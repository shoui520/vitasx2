// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VMManager.h"
#include "Vif_Dynarec.h"
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "vita/VitaGpuVuDirectProgram.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "vita/VitaGpuVuHealthJournal.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuUniversalEpoch.h"
#include "vita/VitaGpuVuVifInput.h"
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
#include "vita/VitaGpuVuOpportunityCensus.h"
#endif
#include "vita/VitaGsMailbox.h"
#include "vita/VitaPerformanceTelemetry.h"
#include "vita/VitaVuBlockCompiler.h"
#include "common/Timer.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

VU_Thread vu1Thread;

#define MTVU_ALWAYS_KICK 0
#define MTVU_SYNC_MODE 0

// A direct affine journal normally collapses to the latest few VU-memory
// ranges. Keep its storage fixed after construction and materialize only when
// an adversarial partial-overlap stream exceeds this bounded first lowering.
static constexpr size_t MaximumDeferredAffineSpans = 64;
static constexpr u32 GpuVuPath1PublishRequest = 1u << 0;
static constexpr u32 GpuVuPath1VisibilityRequest = 1u << 1;
// Look through one bounded frame-scale run before constructing generated
// descriptors. The old three-Execute window forced BSpline's repeating
// generated/generated/generated/state-only chain back through the outer MTVU
// dispatcher about eleven times per frame, even though GXM eventually encoded
// those same objects as one batch. This is only a pointer-free pre-effect
// observation: the transactional in-flight bound below still limits ownership,
// and a failed member leaves the unconsumed suffix in the MTVU ring.
static constexpr u32 MaximumUniversalGpuVuSpeculativeExecuteGather = 47;

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
enum class GpuVuHealthCpu1Stage : u32
{
	Started = 1u,
	BeforeWait,
	AfterWake,
	RetirementPoll,
	RetirementPollReturned,
	DrainBegin,
	DrainSubmitted,
	DrainWaiting,
	DrainComplete,
	DrainFailed,
	Shutdown,
};
#endif

// Deferred exact formulas are observed serially by CPU1. Keep their bounded
// scratch in BSS rather than multiplying heap vectors across every immutable
// accepted-Execute owner. This storage carries no guest state between calls.
alignas(64) static std::array<u32,
	VitaGpuVu::GeneratedLoopKernelEvaluationWorkspaceCapacity>
	s_gpu_vu_deferred_evaluator_values{};
alignas(64) static std::array<u8,
	VitaGpuVu::GeneratedLoopKernelEvaluationWorkspaceCapacity>
	s_gpu_vu_deferred_evaluator_states{};

static VitaGpuVu::GeneratedLoopKernelEvaluationWorkspace
GetGeneratedLoopKernelEvaluationWorkspace()
{
	return {s_gpu_vu_deferred_evaluator_values.data(),
		s_gpu_vu_deferred_evaluator_states.data(),
		s_gpu_vu_deferred_evaluator_values.size()};
}

// Rounds up a size in bytes for size in u32's
static __fi u32 size_u32(u32 x) { return (x + 3) >> 2; }

static __fi u64 MtvuElapsedTelemetryMicroseconds(
	Common::Timer::Value start)
{
	if (start == 0)
		return 0;
	return static_cast<u64>(Common::Timer::ConvertValueToSeconds(
		Common::Timer::GetCurrentValue() - start) * 1000000.0);
}

struct GeneratedRetirementTelemetryScope
{
	GeneratedRetirementTelemetryScope()
		: started(VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0)
	{
	}

	~GeneratedRetirementTelemetryScope()
	{
		if (started != 0)
		{
			VitaGpuVu::RecordGeneratedLoopKernelRetirementPoll(
				MtvuElapsedTelemetryMicroseconds(started));
		}
	}

	Common::Timer::Value started;
};

struct GeneratedCommitTelemetryScope
{
	GeneratedCommitTelemetryScope()
		: started(VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0)
	{
	}

	~GeneratedCommitTelemetryScope()
	{
		if (started != 0)
		{
			VitaGpuVu::RecordGeneratedLoopKernelBatchCommit(
				MtvuElapsedTelemetryMicroseconds(started));
		}
	}

	Common::Timer::Value started;
};

static u64 ExtendGpuVuStateFingerprint(u64 fingerprint, u32 word)
{
	fingerprint ^= word;
	return fingerprint * 1099511628211ull;
}

static u64 FingerprintGpuVuMemory(const u32* memory,
	const u32* unavailable_words, u32 memory_word_count)
{
	if (!memory || !unavailable_words)
		return 0;
	u64 fingerprint = 1469598103934665603ull;
	for (u32 word = 0; word < memory_word_count; word++)
	{
		if ((unavailable_words[word >> 5] & (1u << (word & 31u))) != 0u)
			continue;
		fingerprint = ExtendGpuVuStateFingerprint(fingerprint, word);
		fingerprint = ExtendGpuVuStateFingerprint(fingerprint, memory[word]);
	}
	return fingerprint;
}

static u64 FingerprintGpuVuRegisters(const u32* vf_words,
	const u32* acc_words, const u16* vi_words, u32 q, u32 p, u32 i, u32 tpc)
{
	if (!vf_words || !acc_words || !vi_words)
		return 0;
	u64 fingerprint = 1469598103934665603ull;
	for (u32 word = 0; word < 32u * 4u; word++)
		fingerprint = ExtendGpuVuStateFingerprint(fingerprint, vf_words[word]);
	for (u32 lane = 0; lane < 4u; lane++)
		fingerprint = ExtendGpuVuStateFingerprint(fingerprint, acc_words[lane]);
	for (u32 reg = 0; reg < 16u; reg++)
		fingerprint = ExtendGpuVuStateFingerprint(fingerprint, vi_words[reg]);
	fingerprint = ExtendGpuVuStateFingerprint(fingerprint, q);
	fingerprint = ExtendGpuVuStateFingerprint(fingerprint, p);
	fingerprint = ExtendGpuVuStateFingerprint(fingerprint, i);
	return ExtendGpuVuStateFingerprint(fingerprint, tpc);
}

static bool IsUniversalGpuVuEntryStableRejection(
	VitaGpuVu::UniversalGpuVuRejection rejection)
{
	switch (rejection)
	{
		case VitaGpuVu::UniversalGpuVuRejection::ProgramEncoding:
		case VitaGpuVu::UniversalGpuVuRejection::IncompleteControlFlow:
		case VitaGpuVu::UniversalGpuVuRejection::BranchInDelaySlot:
		case VitaGpuVu::UniversalGpuVuRejection::EnabledDtObserver:
		case VitaGpuVu::UniversalGpuVuRejection::MbitObserver:
		case VitaGpuVu::UniversalGpuVuRejection::UnsupportedConfiguration:
		case VitaGpuVu::UniversalGpuVuRejection::InvalidPairMetadata:
		case VitaGpuVu::UniversalGpuVuRejection::UnsupportedUpper:
		case VitaGpuVu::UniversalGpuVuRejection::UnsupportedLower:
		case VitaGpuVu::UniversalGpuVuRejection::ApproximateQ:
		case VitaGpuVu::UniversalGpuVuRejection::
			GeneratedArchitectureQuarantined:
		case VitaGpuVu::UniversalGpuVuRejection::WatchdogWorkBudget:
				return true;
		default:
			return false;
	}
}

#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
static VitaGpuVu::VifUnpackSpan BuildGpuVuCensusVifSpan(
	const vifStruct& vif, const VIFregisters& vif_regs, u32 source_size)
{
	VitaGpuVu::VifUnpackSpan span;
	span.source_size = source_size;
	span.tag_size_words = vif.tag.size;
	span.mask = vif_regs.mask;
	span.destination_qword = static_cast<u16>(vif.tag.addr >> 4);
	span.vector_count = static_cast<u16>(vif_regs.num);
	span.vif_top = static_cast<u16>(vif_regs.top);
	span.vif_itop = static_cast<u16>(vif_regs.itop);
	span.command = static_cast<u8>(vif.tag.cmd);
	span.cycle_cl = vif_regs.cycle.cl;
	span.cycle_wl = vif_regs.cycle.wl;
	span.mode = static_cast<u8>(vif_regs.mode);
	span.unsigned_data = vif.usn;
	span.start_alignment = vif.start_aligned;
	u32 exact_affine_size = 0;
	if (VitaGpuVu::GetDirectAffineV4_32PayloadSize(
			span, &exact_affine_size))
	{
		span.source_size = exact_affine_size;
	}
	return span;
}
#endif

enum MTVU_EVENT
{
	MTVU_VU_EXECUTE,     // Execute VU program
	MTVU_VU_EXECUTE_DIRECT, // Execute with an admitted direct-program token
	// Cold-compiles an explicit-entry root on the worker, but deliberately
	// retains CpuVU1 execution until its MSCNT continuation state is owned.
	MTVU_VU_EXECUTE_DIRECT_PRIME,
	MTVU_VU_WRITE_MICRO, // Write to VU micro-mem
	MTVU_VU_WRITE_DATA,  // Write to VU data-mem
	MTVU_VU_WRITE_VIREGS,// Write to VU registers
	MTVU_VU_WRITE_VFREGS,// Write to VU registers
	MTVU_VIF_WRITE_COL,  // Write to Vif col reg
	MTVU_VIF_WRITE_ROW,  // Write to Vif row reg
	MTVU_VIF_UNPACK,     // Execute Vif Unpack
	MTVU_VIF_UNPACK_CAPTURED, // Immutable raw VIF payload, deferred to Execute
	MTVU_FLUSH_VIF_UNPACKS,   // Materialize before an external observer
	MTVU_NULL_PACKET,    // Go back to beginning of buffer
	MTVU_RESET
};

// Calls the vif unpack functions from the MTVU thread
static void MTVU_Unpack(void* data, VIFregisters& vifRegs)
{
	u16 wl = vifRegs.cycle.wl > 0 ? vifRegs.cycle.wl : 256;
	bool isFill = vifRegs.cycle.cl < wl;
	if (newVifDynaRec)
		dVifUnpack<1>((u8*)data, isFill);
	else
		_nVifUnpack(1, (u8*)data, vifRegs.mode, isFill);
}

struct GpuVuMemoryView
{
	struct BulkWorkspace
	{
		std::array<const u8*, 1024> qword_sources;
		std::array<u32, 1024> qword_generations;
		// One Execute commonly reads hundreds of distinct qwords from only a
		// handful of immutable VIF UNPACK payloads. ResolveRawVifPayload()
		// validates the ring owner, slot, generation, committed prefix, and
		// reference count with atomic loads. Cache that invariant span base for
		// this view generation; the qword cache below still owns newest-overlay
		// selection and byte-offset validation.
		std::array<const u8*, MaximumDeferredAffineSpans> span_payloads;
		std::array<u32, MaximumDeferredAffineSpans> span_generations;
		u32 generation = 0u;

		void Begin()
		{
			generation++;
			if (generation == 0u)
			{
				qword_generations.fill(0u);
				span_generations.fill(0u);
				generation = 1u;
			}
		}
	};

	const u8* canonical = nullptr;
	const std::vector<VitaGpuVu::VifUnpackSpan>* spans = nullptr;
	const u32* unavailable_memory_words = nullptr;
	BulkWorkspace* bulk_workspace = nullptr;
	// The PCSX2 VU1 data-memory address inside the one permanently GXM-mapped
	// VU region. Private generations may still source unchanged qwords here.
	const u8* gpu_canonical_owner = nullptr;
	const u32* canonical_owner_qwords = nullptr;
	const VitaGpuVu::PersistentVifMemoryProvenance* raw_provenance = nullptr;
};

static const u8* ResolveGpuVuMemorySpanPayload(
	const GpuVuMemoryView& view, size_t span_index)
{
	if (!view.spans || span_index >= view.spans->size())
		return nullptr;
	GpuVuMemoryView::BulkWorkspace* const workspace = view.bulk_workspace;
	if (!workspace || workspace->generation == 0u ||
		span_index >= workspace->span_payloads.size())
	{
		return VitaGpuVu::ResolveRawVifPayload(
			(*view.spans)[span_index].payload);
	}
	if (workspace->span_generations[span_index] != workspace->generation)
	{
		workspace->span_payloads[span_index] =
			VitaGpuVu::ResolveRawVifPayload((*view.spans)[span_index].payload);
		workspace->span_generations[span_index] = workspace->generation;
	}
	return workspace->span_payloads[span_index];
}

static bool BindGpuVuMemoryRawQwords(void* user, u16 first_qword,
	s32 outer_invocation_coefficient, u32 outer_invocation_count,
	s32 child_invocation_coefficient, u32 child_invocation_count,
	VitaGpuVu::RawVifPayloadRef* payload,
	VitaGpuVu::RawQwordBinding* binding)
{
	if (!user)
		return false;
	const GpuVuMemoryView& view =
		*static_cast<const GpuVuMemoryView*>(user);
	return view.raw_provenance && view.raw_provenance->BindGridRawQwords(
		first_qword, outer_invocation_coefficient, outer_invocation_count,
		child_invocation_coefficient, child_invocation_count, payload, binding);
}

static const u8* ResolveGpuVuMemoryQwordSource(
	const GpuVuMemoryView& view, u32 qword)
{
	GpuVuMemoryView::BulkWorkspace* const workspace = view.bulk_workspace;
	if (!workspace || workspace->generation == 0u || qword >= 1024u)
		return nullptr;
	if (workspace->qword_generations[qword] == workspace->generation)
		return workspace->qword_sources[qword];

	const u32 first_word = qword * 4u;
	bool available = view.canonical != nullptr;
	if (available && view.unavailable_memory_words)
	{
		for (u32 lane = 0; lane < 4u; lane++)
		{
			const u32 word = first_word + lane;
			if ((view.unavailable_memory_words[word >> 5] &
				(1u << (word & 31u))) != 0u)
			{
				available = false;
				break;
			}
		}
	}
	const bool canonical_owner = !view.canonical_owner_qwords ||
		(view.canonical_owner_qwords[qword >> 5] &
			(1u << (qword & 31u))) != 0u;
	const u8* source = available ?
		(canonical_owner && view.gpu_canonical_owner ?
			view.gpu_canonical_owner + qword * 16u :
			view.canonical + qword * 16u) : nullptr;
	if (available && view.raw_provenance)
	{
		if (const u8* const raw =
			view.raw_provenance->ResolveQword(static_cast<u16>(qword)))
		{
			source = raw;
		}
	}

	// Resolve only qwords the generated descriptor consumes. Search newest to
	// oldest so the first matching UNPACK is the architectural owner. A
	// malformed newest overlay resolves to nullptr and cannot expose an older
	// value. Rebuilding all 1,024 owners per hot Execute was pure CPU1 cost.
	if (view.spans)
	{
		for (size_t span_index = view.spans->size(); span_index-- > 0u;)
		{
			const VitaGpuVu::VifUnpackSpan& span =
				(*view.spans)[span_index];
			if (!VitaGpuVu::IsDirectAffineV4_32Span(span))
				continue;
			const u32 vector =
				(qword - static_cast<u32>(span.destination_qword)) & 0x3ffu;
			if (vector >= span.vector_count)
				continue;
			const u8* const payload =
				ResolveGpuVuMemorySpanPayload(view, span_index);
			const u32 byte_offset = vector * 16u;
			source = payload && byte_offset <= span.payload.size &&
				16u <= span.payload.size - byte_offset ?
				payload + byte_offset : nullptr;
			break;
		}
	}
	workspace->qword_sources[qword] = source;
	workspace->qword_generations[qword] = workspace->generation;
	return source;
}

static bool ReadGpuVuMemoryQwords(void* user, u16 first_qword,
	u32 qword_count, u32* values)
{
	if (!user || !values || qword_count == 0u)
		return false;
	const GpuVuMemoryView& view =
		*static_cast<const GpuVuMemoryView*>(user);
	for (u32 index = 0; index < qword_count; index++)
	{
		const u8* const source = ResolveGpuVuMemoryQwordSource(
			view, (static_cast<u32>(first_qword) + index) & 0x3ffu);
		if (!source)
			return false;
		std::memcpy(values + index * 4u, source, 16u);
	}
	return true;
}

static bool GpuVuMemoryQwordsHaveCanonicalOwner(
	void* user, u16 first_qword, u32 qword_count)
{
	if (!user || qword_count == 0u ||
		static_cast<u32>(first_qword) + qword_count > 1024u)
	{
		return false;
	}
	const GpuVuMemoryView& view =
		*static_cast<const GpuVuMemoryView*>(user);
	if (!view.gpu_canonical_owner)
	{
		return false;
	}
	for (u32 qword = 0u; qword < qword_count; qword++)
	{
		const u32 address = static_cast<u32>(first_qword) + qword;
		if (ResolveGpuVuMemoryQwordSource(view, address) !=
			view.gpu_canonical_owner + address * 16u)
		{
			return false;
		}
	}
	return true;
}

static bool ReadGpuVuMemoryU32(void* user, u16 qword_address, u8 lane,
	u32* value)
{
	if (!user || !value || lane >= 4)
		return false;
	const GpuVuMemoryView& view =
		*static_cast<const GpuVuMemoryView*>(user);
	const u32 address = static_cast<u32>(qword_address) & 0x3ffu;
	if (view.spans)
	{
		for (size_t span_index = view.spans->size(); span_index-- > 0u;)
		{
			const VitaGpuVu::VifUnpackSpan& span =
				(*view.spans)[span_index];
			if (!VitaGpuVu::IsDirectAffineV4_32Span(span))
				continue;
			const u32 vector =
				(address - static_cast<u32>(span.destination_qword)) & 0x3ffu;
			if (vector >= span.vector_count)
				continue;
			const u8* const payload =
				ResolveGpuVuMemorySpanPayload(view, span_index);
			const u32 byte_offset = vector * 16u + lane * sizeof(u32);
			if (!payload || byte_offset > span.payload.size ||
				sizeof(u32) > span.payload.size - byte_offset)
			{
				return false;
			}
			std::memcpy(value, payload + byte_offset, sizeof(*value));
			return true;
		}
	}
	const u32 word_address = address * 4u + lane;
	if (view.unavailable_memory_words &&
		(view.unavailable_memory_words[word_address >> 5] &
			(1u << (word_address & 31u))) != 0u)
	{
		// A prior generated transaction owns this native SGX output-only
		// lane, but its BUFFER2 journal has not retired.  Returning false is
		// the exact cross-Execute dependency guard: the caller must submit and
		// retire the pending batch before selecting a CPU or GPU successor.
		return false;
	}
	if (view.raw_provenance)
	{
		if (const u8* const raw =
			view.raw_provenance->ResolveQword(static_cast<u16>(address)))
		{
			std::memcpy(value, raw + lane * sizeof(u32), sizeof(*value));
			return true;
		}
	}
	if (!view.canonical)
		return false;
	const u32 byte_address = word_address * sizeof(u32);
	std::memcpy(value, view.canonical + byte_address, sizeof(*value));
	return true;
}

static bool ReadGpuVuMemoryU16(void* user, u16 qword_address, u8 lane,
	u16* value)
{
	if (!value)
		return false;
	u32 word = 0;
	if (!ReadGpuVuMemoryU32(user, qword_address, lane, &word))
		return false;
	*value = static_cast<u16>(word);
	return true;
}

static bool RetainOrMaterializeGpuVuPrivateUnpack(
	VitaGpuVu::PersistentVifMemoryProvenance* provenance,
	const VitaGpuVu::VifUnpackSpan& span, u32* private_memory)
{
	if (!private_memory)
		return false;
	if (provenance && provenance->ApplyDirectAffineSpan(span))
		return true;

	// Owner exhaustion or an internal provenance fault is a performance
	// fallback, never a semantic fallback. Publish every retained raw qword to
	// the private image before dropping the map, then apply the current UNPACK
	// through PCSX2's exact V4-32 materializer.
	if (provenance)
	{
		if (!provenance->MaterializeOwnedQwords(private_memory, VU1_MEMSIZE))
			return false;
		provenance->Clear();
	}
	const u8* const payload = VitaGpuVu::ResolveRawVifPayload(span.payload);
	return payload && VitaGpuVu::MaterializeDirectAffineV4_32Span(
		span, payload, private_memory, VU1_MEMSIZE);
}

// Called on Saving/Loading states...
bool SaveStateBase::mtvuFreeze()
{
	if (!FreezeTag("MTVU"))
		return false;

	if (IsPortableReplay())
	{
		// Portable replay explicitly requires MTVU to be disabled and both VUs
		// idle. In that configuration the worker ring is host-private cache state:
		// encode one canonical empty state and reset it directly on load instead of
		// enqueueing architecture-owned pointers into an inactive worker.
		u32 vu_cycles[4] = {};
		u32 interrupts = 0;
		u64 signal = 0;
		u64 label = 0;
		u32 cycle_index = 0;
		Freeze(vu_cycles);
		Freeze(interrupts);
		Freeze(signal);
		Freeze(label);
		Freeze(cycle_index);
		if (IsLoading())
		{
			for (const u32 cycles : vu_cycles)
			{
				if (cycles != 0)
					m_error = true;
			}
			if (interrupts != 0 || signal != 0 || label != 0 || cycle_index != 0)
				m_error = true;
			if (m_error)
				return false;
			vu1Thread.Reset();
		}
		return IsOkay();
	}

	pxAssert(vu1Thread.IsDone());
	if (!IsSaving())
	{
		vu1Thread.Reset();
		vu1Thread.WriteCol(vif1);
		vu1Thread.WriteRow(vif1);
		vu1Thread.WriteMicroMem(0, VU1.Micro, 0x4000);
		vu1Thread.WriteDataMem(0, VU1.Mem, 0x4000);
		vu1Thread.WriteVIRegs(&VU1.VI[0]);
		vu1Thread.WriteVFRegs(&VU1.VF[0]);
	}
	for (size_t i = 0; i < 4; ++i)
	{
		unsigned int v = vu1Thread.vuCycles[i].load();
		Freeze(v);
	}

	u32 gsInterrupts = vu1Thread.mtvuInterrupts.load();
	Freeze(gsInterrupts);
	vu1Thread.mtvuInterrupts.store(gsInterrupts);
	u64 gsSignal = vu1Thread.gsSignal.load();
	Freeze(gsSignal);
	vu1Thread.gsSignal.store(gsSignal);
	u64 gsLabel = vu1Thread.gsLabel.load();
	Freeze(gsLabel);
	vu1Thread.gsLabel.store(gsLabel);

	Freeze(vu1Thread.vuCycleIdx);
	return IsOkay();
}

VU_Thread::VU_Thread()
{
	static_assert(GeneratedLoopKernelPrivateState::DeferredStoreWordCapacity ==
		VitaGpuVu::GeneratedLoopKernelTransactionOutputWordsPerSlot);
	static_assert(
		GeneratedLoopKernelPrivateState::DeferredMemoryOwnerCapacity <
			GeneratedLoopKernelPrivateState::NativeMemoryOwnerSlotBit);
	static_assert(
		GeneratedLoopKernelPrivateState::NativeMemoryOwnerCapacity <
			GeneratedLoopKernelPrivateState::NativeMemoryOwnerSlotBit);
	m_deferred_vif_unpacks.reserve(MaximumDeferredAffineSpans);
	m_gpu_vu_generated_private_state.deferred_memory_owners.reserve(
		GeneratedLoopKernelPrivateState::DeferredMemoryOwnerCapacity);
	Reset();
	m_gpu_vu_generated_private_state.raw_memory_provenance =
		VitaGpuVu::PersistentVifMemoryProvenance::Create();
}

VU_Thread::~VU_Thread()
{
	Close();
}

void VU_Thread::Open()
{
	if (IsOpen())
		return;

	Reset();
	semaEvent.Reset();
	m_shutdown_flag.store(false, std::memory_order_release);
	m_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	m_thread.Start([this]() { ExecuteRingBuffer(); });
#if defined(__vita__)
	// PCSX2 owner: VMManager::SetEmuThreadAffinities(). Sony's documented
	// application topology is EE=USER_0, VU=USER_1, GS=USER_2. CPU3 remains
	// reserved for the shell, plugins, and system services.
	if (!m_thread.SetAffinity(1u << 1))
		Console.Warning("Vita VU worker affinity was rejected; using the scheduler default.");
#endif
}

void VU_Thread::Close()
{
	if (!IsOpen())
		return;

	WaitVU();
	m_shutdown_flag.store(true, std::memory_order_release);
	semaEvent.NotifyOfWork();
	m_thread.Join();
	ReleaseDeferredVifUnpacks();
}

void VU_Thread::Reset()
{
	ReleaseRetiredUniversalGpuVuEpochs();
	pxAssertRel(m_gpu_vu_universal_pending_epochs.empty(),
		"Reset reached a live universal GPU-VU transaction");
	pxAssertRel(m_gpu_vu_generated_pending_transactions.empty(),
		"Reset reached a live generated GPU-VU transaction");
	pxAssertRel(m_gpu_vu_generated_private_continuations.empty(),
		"Reset reached a live generated GPU-VU private continuation");
	ReleaseDeferredVifUnpacks();
	m_program_active = false;
	m_dt_program_end = false;
	m_pending_program_interrupts = 0;
	m_profile_execute_enqueues = 0;
	m_profile_wait_calls = 0;
	m_profile_ring_waits = 0;
	m_profile_ring_wait_spins = 0;
	m_profile_compile_barriers = 0;
	m_profile_queue_submissions = 0;
	m_profile_queue_words = 0;
	m_micro_write_pending = false;
	m_micro_invalidate_start = 0;
	m_micro_invalidate_end = 0;
	m_gpu_vu_direct_program_token = 0;
	m_gpu_vu_direct_resume_token = 0;
	m_gpu_vu_direct_program_start_pc = 0;
	m_gpu_vu_direct_program_configuration_bits = 0;
	m_gpu_vu_direct_program_prepared = false;
	m_gpu_vu_universal_committed_sequence = 0;
	m_gpu_vu_universal_committed_tpc = 0;
	m_gpu_vu_universal_committed_vf = nullptr;
	m_gpu_vu_universal_committed_state = nullptr;
	m_gpu_vu_universal_committed_memory = nullptr;
	m_gpu_vu_universal_pending_epoch_count.store(0,
		std::memory_order_relaxed);
	m_gpu_vu_generated_pending_transaction_count.store(
		0, std::memory_order_relaxed);
	m_gpu_vu_private_state_load_queued_count = 0;
	m_gpu_vu_private_state_load_adopted_count = 0;
	m_gpu_vu_generated_batch_drain_count = 0;
	m_gpu_vu_generated_async_publication_count = 0;
	m_gpu_vu_generated_unpublished_transactions = 0;
	m_gpu_vu_generated_private_tail_sequence = 0;
	m_gpu_vu_generated_private_retired_transactions = 0;
	m_gpu_vu_generated_private_retired_pairs = 0;
	m_gpu_vu_generated_private_completion_count = 0;
	m_gpu_vu_generated_private_completion_cycles.fill(0u);
	m_gpu_vu_private_state_bridge_quarantined = false;
	InvalidateGeneratedLoopKernelPrivateState();
	m_gpu_vu_micro_generation = 1;
	m_gpu_vu_generated_generation_had_product = false;
	m_gpu_vu_dispatch_cost_cache = {};
	m_gpu_vu_dispatch_cost_cache_next = 0;
	m_gpu_vu_path1_publication_requests.store(0u, std::memory_order_relaxed);
	m_vif_span_sequence = 0;
	m_pending_vif_batch = false;
	m_pending_captured_vif_span_pos = -1;
	vuCycleIdx = 0;
	m_ato_write_pos = 0;
	m_write_pos = 0;
	m_ato_read_pos = 0;
	m_read_pos = 0;
	std::memset(&vif, 0, sizeof(vif));
	std::memset(&vifRegs, 0, sizeof(vifRegs));
	for (size_t i = 0; i < 4; ++i)
		vu1Thread.vuCycles[i] = 0;
	vu1Thread.mtvuInterrupts = 0;
}

void VU_Thread::RebuildFromCanonicalStateAfterPortableLoad()
{
	pxAssert(IsOpen());
	pxAssert(IsDone());
	Reset();
	WriteCol(vif1);
	WriteRow(vif1);
	WriteMicroMem(0, VU1.Micro, VU1_PROGSIZE);
	WriteDataMem(0, VU1.Mem, VU1_MEMSIZE);
	WriteVIRegs(&VU1.VI[0]);
	WriteVFRegs(&VU1.VF[0]);
}

void VU_Thread::BeginProgram()
{
	m_program_active = true;
	m_dt_program_end = false;
	m_pending_program_interrupts = 0;
}

void VU_Thread::MarkDtProgramEnd(u32 interrupt_flag)
{
	m_dt_program_end = true;
	m_pending_program_interrupts |= interrupt_flag;
}

void VU_Thread::EndProgram(u32 interrupt_flag)
{
	// PCSX2 owner: x86/microVU_Branch.inl publishes E and T through
	// mVUEBit()/mVUTBit(). Its MTVU path uses the VUTBit handoff for either
	// enabled D or T because both stop VU1 and raise the EE-side VU1 event.
	// A D/T exit takes precedence over a static E bit on the same pair.
	if (m_dt_program_end)
		interrupt_flag &= ~InterruptFlagVUEBit;
	interrupt_flag |= m_pending_program_interrupts;
	m_pending_program_interrupts = 0;
	m_dt_program_end = false;
	m_program_active = false;
	if (interrupt_flag != 0)
		mtvuInterrupts.fetch_or(interrupt_flag, std::memory_order_release);
}

void VU_Thread::NotifyUniversalGpuVuProgress()
{
	semaEvent.NotifyOfWork();
}

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
void VU_Thread::PublishGpuVuHealth(u32 stage, u32 wait_poll_count)
{
	VitaGpuVu::HealthJournal::Cpu1State health;
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	health.update_time_us = static_cast<u64>(
		Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
	if (!m_gpu_vu_generated_pending_transactions.empty())
	{
		const auto& front = m_gpu_vu_generated_pending_transactions.front();
		const auto& back = m_gpu_vu_generated_pending_transactions.back();
		health.sequence_front = front ? front->Sequence() : 0u;
		health.sequence_back = back ? back->Sequence() : 0u;
	}
	else if (!m_gpu_vu_generated_private_continuations.empty())
	{
		health.sequence_front =
			m_gpu_vu_generated_private_continuations.front().predecessor_sequence;
		health.sequence_back =
			m_gpu_vu_generated_private_continuations.back().sequence;
	}
	health.private_generation = m_gpu_vu_generated_private_tail_sequence;
	health.stage = stage;
	health.pending_physical_count = static_cast<u32>(
		m_gpu_vu_generated_pending_transactions.size());
	health.pending_logical_count = GeneratedLoopKernelPendingExecutionCount();
	health.private_transaction_count = static_cast<u32>(std::min<u64>(
		m_gpu_vu_generated_private_completion_count,
		std::numeric_limits<u32>::max()));
	health.private_generation_valid =
		m_gpu_vu_generated_private_state.valid ? 1u : 0u;
	health.wait_poll_count = wait_poll_count;
	health.flags =
		(m_gpu_vu_generated_unpublished_transactions != 0u ? 1u : 0u) |
		(m_gpu_vu_private_state_bridge_quarantined ? (1u << 1u) : 0u) |
		(m_pending_vif_batch ? (1u << 2u) : 0u) |
		(m_shutdown_flag.load(std::memory_order_relaxed) ? (1u << 3u) : 0u);
	VitaGpuVu::HealthJournal::PublishCpu1(health);
}
#endif

void VU_Thread::ReleaseRetiredUniversalGpuVuEpochs()
{
	while (!m_gpu_vu_universal_pending_epochs.empty())
	{
		PendingUniversalGpuVuEpoch& pending =
			m_gpu_vu_universal_pending_epochs.front();
		const VitaGpuVu::UniversalGpuVuEpochStage stage =
			pending.epoch->Stage();
		if (!pending.execution_published ||
			(stage != VitaGpuVu::UniversalGpuVuEpochStage::Retired &&
			 stage != VitaGpuVu::UniversalGpuVuEpochStage::Cancelled))
		{
			break;
		}
		for (VitaGpuVu::VifUnpackSpan& span : pending.replay_unpacks)
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
		m_gpu_vu_universal_pending_epochs.pop_front();
		m_gpu_vu_universal_pending_epoch_count.store(
			static_cast<u32>(m_gpu_vu_universal_pending_epochs.size()),
			std::memory_order_release);
	}
}

bool VU_Thread::DrainUniversalGpuVuEpochs(bool wait_for_all)
{
	if (!DrainGeneratedLoopKernelTransactions(
			wait_for_all, wait_for_all))
		return false;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	for (;;)
	{
		ReleaseRetiredUniversalGpuVuEpochs();
		if (m_gpu_vu_universal_pending_epochs.empty())
			return true;

		PendingUniversalGpuVuEpoch& pending =
			m_gpu_vu_universal_pending_epochs.front();
		VitaGpuVu::UniversalGpuVuEpoch& epoch = *pending.epoch;
		const VitaGpuVu::UniversalGpuVuEpochStage stage = epoch.Stage();
		if (pending.execution_published)
		{
			if (!wait_for_all)
				return true;
			epoch.WaitForStageChange(stage);
			continue;
		}

		if (stage == VitaGpuVu::UniversalGpuVuEpochStage::Prepared ||
			stage == VitaGpuVu::UniversalGpuVuEpochStage::Completing ||
			stage == VitaGpuVu::UniversalGpuVuEpochStage::Submitted)
		{
			if (!wait_for_all)
				return true;
			epoch.WaitForStageChange(stage);
			continue;
		}

		if (stage == VitaGpuVu::UniversalGpuVuEpochStage::Accepted)
		{
			const VitaGpuVu::UniversalGpuVuCompletionRecord& completion =
				epoch.Completion();
			const VitaGpuVu::UniversalGpuVuCommittedStateView& committed =
				epoch.CommittedState();
			if (!committed.IsValid() || committed.sequence != epoch.Sequence())
			{
				Console.Error(
					"GPU-VU: accepted sequence %llu published no stable mapped state.",
					static_cast<unsigned long long>(epoch.Sequence()));
				return false;
			}

			m_gpu_vu_universal_committed_sequence = epoch.Sequence();
			m_gpu_vu_universal_committed_tpc = completion.final_tpc;
			m_gpu_vu_universal_committed_vf = committed.vf_words;
			m_gpu_vu_universal_committed_state = committed.state_words;
			m_gpu_vu_universal_committed_memory = committed.vu_memory;
			for (VitaGpuVu::VifUnpackSpan& span : pending.replay_unpacks)
				VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			pending.replay_unpacks.clear();

			BeginProgram();
			EndProgram(completion.interrupt_flags);
			for (u32 execute_index = 0; execute_index < epoch.ExecuteCount();
				execute_index++)
			{
				vuCycles[vuCycleIdx].store(
					completion.final_cycle, std::memory_order_release);
				vuCycleIdx = (vuCycleIdx + 1) & 3;
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
				Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
			}

			pending.execution_published = true;
			if (!epoch.MarkAcceptedStateAcquired())
			{
				Console.Error(
					"GPU-VU: CPU1 could not acquire accepted sequence %llu.",
					static_cast<unsigned long long>(epoch.Sequence()));
				return false;
			}
			m_execute_jobs_completed.fetch_add(
				epoch.ExecuteCount(), std::memory_order_release);
			m_ring_space_progress.NotifyOfProgress();
			if (pending.attempt_started_at != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuWorkerAttemptTime(
					MtvuElapsedTelemetryMicroseconds(
						pending.attempt_started_at));
			}
			VitaGS::NotifyUniversalGpuVuProgress();
			continue;
		}

		if (stage == VitaGpuVu::UniversalGpuVuEpochStage::GpuRejected)
		{
			const Common::Timer::Value materialize_start =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			if (!MaterializeUniversalGpuVuStateForCpu())
				return false;
			if (materialize_start != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuCpuMaterializeTime(
					MtvuElapsedTelemetryMicroseconds(materialize_start));
			}

			if (pending.addr != -1)
				VU1.VI[REG_TPC].UL = static_cast<u32>(pending.addr) & 0x7ffu;
			CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
			VU1.cycle = 0;
			const Common::Timer::Value unpack_start =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			ReplayVifUnpackSpans(&pending.replay_unpacks);
			if (unpack_start != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuCpuUnpackReplayTime(
					MtvuElapsedTelemetryMicroseconds(unpack_start));
			}
			vifRegs.top = pending.vif_top;
			vifRegs.itop = pending.vif_itop;
			vuFBRST = pending.fbrst;
			std::array<u16, 16> cold_initial_vi{};
			for (u32 reg = 0; reg < cold_initial_vi.size(); reg++)
				cold_initial_vi[reg] = VU1.VI[reg].US[0];
			BeginProgram();
			VitaVU::Vu1StructuredBoundaryTrace structured_boundary_trace;
			const bool structured_preflight_diagnostic =
				epoch.Rejection() ==
					VitaGpuVu::UniversalGpuVuRejection::
						RuntimeStructuredPreflight &&
				epoch.StructuredBundle().HasKeys();
			const bool structured_boundary_trace_started =
				(epoch.HasPrivateStructuredResult() ||
				 structured_preflight_diagnostic) &&
				VitaVU::BeginVu1StructuredBoundaryTrace(
					epoch.StructuredBundle().entry_pc,
					epoch.StructuredBundle().child_entry_pc);
			const Common::Timer::Value fallback_start =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			CpuVU1->Execute(vu1RunCycles);
			const bool structured_boundary_trace_finished =
				structured_boundary_trace_started &&
				VitaVU::EndVu1StructuredBoundaryTrace(
					&structured_boundary_trace);
			if (fallback_start != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuCpuFallbackTime(
					MtvuElapsedTelemetryMicroseconds(fallback_start));
			}
			const u32 path1_bytes = gifUnit.gifPath[GIF_PATH_1].gsPack.size;
			const Gif_Path& cold_path1 = gifUnit.gifPath[GIF_PATH_1];
			const bool cold_path1_range_valid =
				cold_path1.buffer &&
				cold_path1.gsPack.offset <= cold_path1.buffSize &&
				path1_bytes <= cold_path1.buffSize - cold_path1.gsPack.offset;
			if (path1_bytes >= sizeof(std::array<u32, 4>) &&
				cold_path1_range_valid)
			{
				std::array<u32, 4> cold_gif_tag{};
				std::memcpy(cold_gif_tag.data(),
					cold_path1.buffer + cold_path1.gsPack.offset,
					sizeof(cold_gif_tag));
				VitaGpuVu::RecordGeneratedNestedDirectPath1Tag(
					epoch.ProgramIdentity(), cold_gif_tag, &cold_initial_vi,
					static_cast<u16>(pending.vif_top),
					static_cast<u16>(pending.vif_itop));
			}
			else
			{
				VitaGpuVu::RecordGeneratedNestedDirectNoPath1(
					epoch.ProgramIdentity());
			}
			if (structured_preflight_diagnostic &&
				structured_boundary_trace_finished)
			{
				const VitaGpuVu::StructuredGeneratedBundle& bundle =
					epoch.StructuredBundle();
				const u32 child_counter_reg =
					bundle.memory_preflight.header1[1];
				const u32 child_limit_reg =
					bundle.memory_preflight.header1[2];
				const s32 child_step = static_cast<s32>(
					bundle.memory_preflight.header1[3]);
				const auto child_trip = [child_counter_reg, child_limit_reg,
					child_step](
					const VitaVU::Vu1StructuredBoundarySnapshot& snapshot) {
					const u32 counter = snapshot.vi[child_counter_reg] & 0xffffu;
					const u32 limit = child_limit_reg == 0u ? 0u :
						snapshot.vi[child_limit_reg] & 0xffffu;
					return child_step > 0 ? (limit - counter) & 0xffffu :
						(counter - limit) & 0xffffu;
				};
				const VitaVU::Vu1StructuredBoundarySnapshot* const first =
					structured_boundary_trace.snapshots.empty() ? nullptr :
						&structured_boundary_trace.snapshots.front();
				const VitaVU::Vu1StructuredBoundarySnapshot* const last =
					structured_boundary_trace.snapshots.empty() ? nullptr :
						&structured_boundary_trace.snapshots.back();
				Console.Warning(
					"GPU-VU seq=%llu provider=generated-structured "
					"preflight-cpu-oracle parent_observations=%u snapshots=%u "
					"dropped=%u executed_pairs=%u outer_bound=%u child_bound=%u "
					"child_control=%u,%u,%d first_outer=%u first_counter=%u "
					"first_limit=%u first_trip=%u last_outer=%u last_counter=%u "
					"last_limit=%u last_trip=%u canonical=cpu-replay",
					static_cast<unsigned long long>(epoch.Sequence()),
					structured_boundary_trace.parent_observations,
					static_cast<u32>(structured_boundary_trace.snapshots.size()),
					structured_boundary_trace.dropped_snapshots,
					structured_boundary_trace.executed_pairs,
					bundle.maximum_outer_iterations,
					bundle.maximum_child_iterations, child_counter_reg,
					child_limit_reg, child_step,
					first ? first->outer_iteration : 0u,
					first ? first->vi[child_counter_reg] & 0xffffu : 0u,
					first && child_limit_reg != 0u ?
						first->vi[child_limit_reg] & 0xffffu : 0u,
					first ? child_trip(*first) : 0u,
					last ? last->outer_iteration : 0u,
					last ? last->vi[child_counter_reg] & 0xffffu : 0u,
					last && child_limit_reg != 0u ?
						last->vi[child_limit_reg] & 0xffffu : 0u,
					last ? child_trip(*last) : 0u);
			}
			if (epoch.HasPrivateStructuredResult())
			{
				if (structured_boundary_trace_finished &&
					!structured_boundary_trace.snapshots.empty())
				{
					const VitaVU::Vu1StructuredBoundarySnapshot& first =
						structured_boundary_trace.snapshots.front();
					Console.Warning(
						"GPU-VU seq=%llu provider=generated-structured "
						"cpu-child-diagnostic outer=%u "
						"vf1=%08x:%08x:%08x:%08x "
						"vf13=%08x:%08x:%08x:%08x vi10=%u "
						"cycle=%llu canonical=cpu-replay",
						static_cast<unsigned long long>(epoch.Sequence()),
						first.outer_iteration, first.vf[1u * 4u + 0u],
						first.vf[1u * 4u + 1u], first.vf[1u * 4u + 2u],
						first.vf[1u * 4u + 3u], first.vf[13u * 4u + 0u],
						first.vf[13u * 4u + 1u], first.vf[13u * 4u + 2u],
						first.vf[13u * 4u + 3u], first.vi[10],
						static_cast<unsigned long long>(first.cycle));
				}
				const VitaGpuVu::UniversalGpuVuStructuredBoundaryComparison
					boundary_comparison = structured_boundary_trace_finished ?
						epoch.ComparePrivateStructuredBoundaryTrace(
							structured_boundary_trace) :
						VitaGpuVu::UniversalGpuVuStructuredBoundaryComparison{};
				Console.Warning(
					"GPU-VU seq=%llu provider=generated-structured "
					"child-entry-oracle=%s available=%u "
					"analysis_pc=%u parent_pc=%u child_pc=%u "
					"gpu_outer=%u cpu_outer=%u "
					"cpu_parent=%u cpu_pairs=%u first_outer=%u component=%s "
					"reg=%u lane=%u gpu=%08x cpu=%08x "
					"prior_vector=%u prior_gpu=%08x:%08x:%08x:%08x "
					"prior_cpu=%08x:%08x:%08x:%08x canonical=cpu-replay",
					static_cast<unsigned long long>(epoch.Sequence()),
					boundary_comparison.available ?
						(boundary_comparison.exact ? "exact" : "MISMATCH") :
						"unavailable",
					static_cast<u32>(boundary_comparison.available),
					epoch.AnalysisEntryPc(),
					boundary_comparison.parent_entry_pc,
					boundary_comparison.child_entry_pc,
					boundary_comparison.gpu_outer_iterations,
					boundary_comparison.cpu_outer_iterations,
					boundary_comparison.cpu_parent_observations,
					boundary_comparison.cpu_executed_pairs,
					boundary_comparison.first_outer_iteration,
					VitaGpuVu::UniversalGpuVuStructuredBoundaryComponentName(
						boundary_comparison.component),
					boundary_comparison.register_index,
					boundary_comparison.lane,
					boundary_comparison.gpu_word,
					boundary_comparison.cpu_word,
					static_cast<u32>(boundary_comparison.prior_vector_available),
					boundary_comparison.prior_gpu_vector[0],
					boundary_comparison.prior_gpu_vector[1],
					boundary_comparison.prior_gpu_vector[2],
					boundary_comparison.prior_gpu_vector[3],
					boundary_comparison.prior_cpu_vector[0],
					boundary_comparison.prior_cpu_vector[1],
					boundary_comparison.prior_cpu_vector[2],
					boundary_comparison.prior_cpu_vector[3]);
				const Gif_Path& path1 = gifUnit.gifPath[GIF_PATH_1];
				const bool path1_range_valid = path1_bytes == 0 ||
					(path1.buffer && path1.gsPack.offset <= path1.buffSize &&
					 path1_bytes <= path1.buffSize - path1.gsPack.offset);
				const u8* const path1_data = path1_bytes != 0 &&
					path1_range_valid ? path1.buffer + path1.gsPack.offset : nullptr;
				const VitaGpuVu::UniversalGpuVuStructuredComparison comparison =
					epoch.ComparePrivateStructuredResult(
						&VU1, path1_data, path1_range_valid ? path1_bytes : 0u);
				Console.Warning(
					"GPU-VU seq=%llu provider=generated-structured "
					"oracle_attestation=%s vf=%u state=%u memory=%u path1=%u "
					"pairs=%u gpu_path1=%u/%u cpu_path1_bytes=%u "
					"first_vf=%u:%08x/%08x first_state=%u:%08x/%08x "
					"first_memory=%u:%08x/%08x first_path1=%u:%08x/%08x "
					"canonical=cpu-replay",
					static_cast<unsigned long long>(epoch.Sequence()),
					comparison.Exact() ? "exact" : "MISMATCH",
					static_cast<u32>(comparison.vf_exact),
					static_cast<u32>(comparison.state_exact),
					static_cast<u32>(comparison.memory_exact),
					static_cast<u32>(comparison.path1_exact),
					comparison.gpu_executed_pairs,
					comparison.gpu_path1_packets,
					comparison.gpu_path1_qwords,
					comparison.cpu_path1_bytes,
					comparison.first_vf_word, comparison.gpu_vf_word,
					comparison.cpu_vf_word, comparison.first_state_word,
					comparison.gpu_state_word, comparison.cpu_state_word,
					comparison.first_memory_word,
					comparison.gpu_memory_word, comparison.cpu_memory_word,
					comparison.first_path1_word,
					comparison.gpu_path1_word, comparison.cpu_path1_word);
			}
			if (epoch.Rejection() ==
				VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1)
			{
				const u32 address = epoch.RejectedPath1Address();
				std::array<u32, 4> cpu_tag{};
				std::memcpy(cpu_tag.data(), VU1.Mem + address,
					cpu_tag.size() * sizeof(u32));
				const auto& gpu_tag = epoch.RejectedPath1Tag();
				Console.Warning(
					"GPU-VU seq=%llu provider=universal replay-path1-oracle "
					"address=%04x gpu_tag=%08x:%08x:%08x:%08x "
					"cpu_tag=%08x:%08x:%08x:%08x cpu_path1_bytes=%u",
					static_cast<unsigned long long>(epoch.Sequence()), address,
					gpu_tag[0], gpu_tag[1], gpu_tag[2], gpu_tag[3], cpu_tag[0],
					cpu_tag[1], cpu_tag[2], cpu_tag[3], path1_bytes);
			}
			VitaGpuVu::RecordCpuVu1Execution(path1_bytes);
			const Common::Timer::Value path1_start =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
			if (path1_start != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuCpuPath1FinishTime(
					MtvuElapsedTelemetryMicroseconds(path1_start));
			}
			vuCycles[vuCycleIdx].store(VU1.cycle, std::memory_order_release);
			vuCycleIdx = (vuCycleIdx + 1) & 3;
			VitaGpuVu::RecordUniversalGpuVuCpuFallback(
				pending.classified_pairs);
			pending.execution_published = true;
			if (!epoch.MarkCpuFallback(
					epoch.Rejection(), pending.classified_pairs))
			{
				Console.Error(
					"GPU-VU: rejected sequence %llu could not publish CPU replay.",
					static_cast<unsigned long long>(epoch.Sequence()));
				return false;
			}
			m_execute_jobs_completed.fetch_add(
				epoch.ExecuteCount(), std::memory_order_release);
			m_ring_space_progress.NotifyOfProgress();
			if (pending.attempt_started_at != 0)
			{
				VitaGpuVu::RecordUniversalGpuVuWorkerAttemptTime(
					MtvuElapsedTelemetryMicroseconds(
						pending.attempt_started_at));
			}
			VitaGS::NotifyUniversalGpuVuProgress();
			continue;
		}

		if (stage == VitaGpuVu::UniversalGpuVuEpochStage::Cancelled)
		{
			pending.execution_published = true;
			ReleaseRetiredUniversalGpuVuEpochs();
			return false;
		}

		Console.Error("GPU-VU: sequence %llu reached invalid pending stage %u.",
			static_cast<unsigned long long>(epoch.Sequence()),
			static_cast<u32>(stage));
		return false;
	}
#else
	(void)wait_for_all;
	return true;
#endif
}

#if defined(VITASX2_QEMU_VALIDATION)
bool VU_Thread::ValidateGeneratedLoopKernelDeferredRegisterOwners()
{
	class FixedDeferredSuccessor final : public
		VitaGpuVu::GeneratedLoopKernelDeferredSuccessor
	{
	public:
		explicit FixedDeferredSuccessor(u32 marker) : m_marker(marker) {}

		bool Evaluate(VitaGpuVu::GeneratedLoopKernelFinalStateValues* values,
			VitaGpuVu::GeneratedLoopKernelEvaluationWorkspace,
			std::string* error) const override
		{
			if (!values)
				return false;
			for (auto& reg : values->vf)
				reg.fill(m_marker);
			values->acc.fill(m_marker);
			values->q = m_marker;
			values->p = m_marker;
			values->i = m_marker;
			if (error)
				error->clear();
			return true;
		}

	private:
		u32 m_marker;
	};

	const auto make_owner = [](u32 marker) {
		return std::make_shared<const FixedDeferredSuccessor>(marker);
	};
	const auto mark_unavailable = [](GeneratedLoopKernelPrivateState* state,
			const std::array<u8, 32>& vf_lanes, u8 acc_lanes,
			bool final_q, bool final_p, bool final_i) {
		if (!state)
			return;
		for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
			state->unavailable_vf_lanes[reg] |= vf_lanes[reg];
		state->unavailable_acc_lanes |= acc_lanes;
		if (final_q)
			state->q_available = false;
		if (final_p)
			state->p_available = false;
		if (final_i)
			state->i_available = false;
	};
	const auto live_owner_count = [](const GeneratedLoopKernelPrivateState& state) {
		return static_cast<u32>(std::count_if(
			state.deferred_register_owners.begin(),
			state.deferred_register_owners.end(), [](const auto& retained) {
				return retained.live_reference_count != 0u;
			}));
	};

	GeneratedLoopKernelPrivateState state;
	std::array<u8, 32> complete_vf_lanes{};
	for (u32 reg = 1u; reg < complete_vf_lanes.size(); reg++)
		complete_vf_lanes[reg] = 0x0fu;
	const auto first = make_owner(1u);
	if (!state.CanAssignDeferredRegisterOwner(
			first, complete_vf_lanes, 0x0fu, true, true, true) ||
		!state.AssignDeferredRegisterOwner(
			first, complete_vf_lanes, 0x0fu, true, true, true))
	{
		return false;
	}
	mark_unavailable(
		&state, complete_vf_lanes, 0x0fu, true, true, true);
	const auto original_vf_slots = state.deferred_vf_owner_slots;
	const auto original_acc_slots = state.deferred_acc_owner_slots;
	const u8 original_q_slot = state.deferred_q_owner_slot;
	const u8 first_slot = state.FindDeferredRegisterOwner(first);
	if (first_slot == 0u || live_owner_count(state) != 1u ||
		state.deferred_register_owners[first_slot - 1u].live_reference_count !=
			31u * 4u + 4u + 3u)
	{
		return false;
	}

	// A complete successor replaces one retained pointer and leaves all compact
	// lane IDs untouched.
	const auto second = make_owner(2u);
	if (!state.CanAssignDeferredRegisterOwner(
			second, complete_vf_lanes, 0x0fu, true, true, true) ||
		!state.AssignDeferredRegisterOwner(
			second, complete_vf_lanes, 0x0fu, true, true, true) ||
		state.deferred_vf_owner_slots != original_vf_slots ||
		state.deferred_acc_owner_slots != original_acc_slots ||
		state.deferred_q_owner_slot != original_q_slot ||
		state.DeferredRegisterOwnerForSlot(original_q_slot).get() != second.get())
	{
		return false;
	}

	// A partial overwrite splits ownership exactly, then a complete write may
	// supersede both partitions without rewriting any lane IDs.
	std::array<u8, 32> one_lane{};
	one_lane[1] = 0x08u;
	const auto third = make_owner(3u);
	if (!state.CanAssignDeferredRegisterOwner(
			third, one_lane, 0u, false, false, false) ||
		!state.AssignDeferredRegisterOwner(
			third, one_lane, 0u, false, false, false))
	{
		return false;
	}
	mark_unavailable(&state, one_lane, 0u, false, false, false);
	const u8 third_slot = state.deferred_vf_owner_slots[4u];
	if (third_slot == 0u || third_slot == original_vf_slots[4u] ||
		live_owner_count(state) != 2u ||
		state.DeferredRegisterOwnerForSlot(third_slot).get() != third.get())
	{
		return false;
	}
	const auto fourth = make_owner(4u);
	if (!state.CanAssignDeferredRegisterOwner(
			fourth, complete_vf_lanes, 0x0fu, true, true, true) ||
		!state.AssignDeferredRegisterOwner(
			fourth, complete_vf_lanes, 0x0fu, true, true, true))
	{
		return false;
	}
	for (u32 reg = 1u; reg < complete_vf_lanes.size(); reg++)
	{
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			if (state.DeferredRegisterOwnerForSlot(
					state.deferred_vf_owner_slots[reg * 4u + lane]).get() !=
				fourth.get())
			{
				return false;
			}
		}
	}

	// Exhaustion is a pre-effect rejection. A seventeenth independent owner
	// cannot consume an available lane, but it can replace a fully covered old
	// lane and reuse that exact slot.
	state.ResetDeferredRegisterOwners();
	state.unavailable_vf_lanes.fill(0u);
	state.unavailable_acc_lanes = 0u;
	state.q_available = true;
	state.p_available = true;
	state.i_available = true;
	for (u32 index = 0u;
		index < GeneratedLoopKernelPrivateState::DeferredRegisterOwnerCapacity;
		index++)
	{
		std::array<u8, 32> lane{};
		lane[1u + index / 4u] = static_cast<u8>(0x8u >> (index & 3u));
		const auto owner = make_owner(16u + index);
		if (!state.CanAssignDeferredRegisterOwner(
				owner, lane, 0u, false, false, false) ||
			!state.AssignDeferredRegisterOwner(
				owner, lane, 0u, false, false, false))
		{
			return false;
		}
		mark_unavailable(&state, lane, 0u, false, false, false);
	}
	if (live_owner_count(state) !=
		GeneratedLoopKernelPrivateState::DeferredRegisterOwnerCapacity)
	{
		return false;
	}
	std::array<u8, 32> available_lane{};
	available_lane[5] = 0x08u;
	const auto overflow = make_owner(99u);
	if (state.CanAssignDeferredRegisterOwner(
			overflow, available_lane, 0u, false, false, false))
	{
		return false;
	}
	std::array<u8, 32> replacement_lane{};
	replacement_lane[1] = 0x08u;
	if (!state.CanAssignDeferredRegisterOwner(
			overflow, replacement_lane, 0u, false, false, false) ||
		!state.AssignDeferredRegisterOwner(
			overflow, replacement_lane, 0u, false, false, false))
	{
		return false;
	}
	mark_unavailable(&state, replacement_lane, 0u, false, false, false);
	return live_owner_count(state) ==
			GeneratedLoopKernelPrivateState::DeferredRegisterOwnerCapacity &&
		state.DeferredRegisterOwnerForSlot(
			state.deferred_vf_owner_slots[4u]).get() == overflow.get();
}

bool VU_Thread::ValidateGeneratedLoopKernelLazyNativeMemoryOwners() {
  using Layout = VitaGpuVu::GeneratedLoopKernelTransactionLayout;
  GeneratedLoopKernelPrivateState state;
  Layout full_layout;
  Layout prefix_layout;
  std::array<u32, GeneratedLoopKernelPrivateState::MemoryMaskWordCount>
      full_mask{};
  std::array<u32, GeneratedLoopKernelPrivateState::MemoryMaskWordCount>
      prefix_mask{};
  full_mask[3] = 0x000000ffu;
  prefix_mask[3] = 0x0000000fu;

  const u8 full_slot = state.RetainNativeMemoryOwner(1u, &full_layout, 8u);
  if (full_slot == 0u)
    return false;
  state.SetUnavailableMemoryOwnerMask(full_mask, 1u, full_slot);
  if (state.LiveNativeMemoryOwnerWordCount(1u) != 8u ||
      !state.MarkNativeMemoryOwnerMaterialized(1u)) {
    return false;
  }

  // A same-layout generated successor must invalidate the old copied value
  // while retaining the exact compact lane partition.
  if (state.ReplaceFullyLiveNativeMemoryOwner(2u, &full_layout, 8u) !=
          full_slot ||
      state.LiveNativeMemoryOwnerWordCount(1u) != 0u ||
      state.LiveNativeMemoryOwnerWordCount(2u) != 8u ||
      state.PublishMaterializedNativeMemoryOwners()) {
    return false;
  }
  if (!state.MarkNativeMemoryOwnerMaterialized(2u))
    return false;

  // A smaller output splits the live partition. A later full output can adopt
  // both partitions without changing any lane slot, but neither partition may
  // publish until that replacement transaction has completed.
  const u8 prefix_slot = state.RetainNativeMemoryOwner(3u, &prefix_layout, 4u);
  if (prefix_slot == 0u || prefix_slot == full_slot)
    return false;
  state.SetUnavailableMemoryOwnerMask(prefix_mask, 3u, prefix_slot);
  if (state.LiveNativeMemoryOwnerWordCount(2u) != 4u ||
      state.LiveNativeMemoryOwnerWordCount(3u) != 4u ||
      !state.MarkNativeMemoryOwnerMaterialized(3u)) {
    return false;
  }
  u32 replaced_owners = 0u;
  if (!state.ReplaceCoveredNativeMemoryOwners(4u, &full_layout, 8u, full_mask,
                                              &replaced_owners) ||
      replaced_owners != 2u || state.LiveNativeMemoryOwnerWordCount(4u) != 8u) {
    return false;
  }
  const auto unavailable_before_failed_publish = state.unavailable_memory_words;
  const auto slots_before_failed_publish = state.unavailable_memory_owner_slot;
  if (state.PublishMaterializedNativeMemoryOwners() ||
      state.unavailable_memory_words != unavailable_before_failed_publish ||
      state.unavailable_memory_owner_slot != slots_before_failed_publish ||
      !state.MarkNativeMemoryOwnerMaterialized(4u) ||
      !state.PublishMaterializedNativeMemoryOwners()) {
    return false;
  }
  const bool owner_publication_clean =
      std::all_of(state.unavailable_memory_words.begin(),
                  state.unavailable_memory_words.end(),
                  [](u32 mask) { return mask == 0u; }) &&
      std::all_of(state.unavailable_memory_owner_slot.begin(),
                  state.unavailable_memory_owner_slot.end(),
                  [](u8 slot) { return slot == 0u; }) &&
      state.LiveNativeMemoryOwnerWordCount(4u) == 0u;

  // Direct V4-32 ownership clearing must preserve the architectural 16 KiB
  // wrap while dropping exactly the overwritten native owner lanes.  Exercise
  // both the non-mutating preflight mask helper and the committing owner map.
  GeneratedLoopKernelPrivateState unpack_state;
  unpack_state.canonical_memory_qwords.fill(~0u);
  std::array<u32, GeneratedLoopKernelPrivateState::MemoryMaskWordCount>
      wrapped_mask{};
  wrapped_mask.front() = 0x0000000fu;
  wrapped_mask.back() = 0xf0000000u;
  const u8 wrapped_slot =
      unpack_state.RetainNativeMemoryOwner(5u, &full_layout, 8u);
  if (wrapped_slot == 0u)
    return false;
  unpack_state.SetUnavailableMemoryOwnerMask(
      wrapped_mask, 5u, wrapped_slot);
  auto preflight_mask = wrapped_mask;
  GeneratedLoopKernelPrivateState::MarkCompleteQwordSpanAvailable(
      &preflight_mask, 0x3ffu, 2u);
  unpack_state.ApplyCompleteQwordUnpackOwnership(0x3ffu, 2u);
  const bool wrapped_unpack_clean =
      std::all_of(preflight_mask.begin(), preflight_mask.end(),
                  [](u32 mask) { return mask == 0u; }) &&
      std::all_of(unpack_state.unavailable_memory_words.begin(),
                  unpack_state.unavailable_memory_words.end(),
                  [](u32 mask) { return mask == 0u; }) &&
      std::all_of(unpack_state.unavailable_memory_owner_slot.begin(),
                  unpack_state.unavailable_memory_owner_slot.end(),
                  [](u8 slot) { return slot == 0u; }) &&
      unpack_state.LiveNativeMemoryOwnerWordCount(5u) == 0u &&
      (unpack_state.canonical_memory_qwords.front() & 0x1u) == 0u &&
      (unpack_state.canonical_memory_qwords.back() & 0x80000000u) == 0u &&
      (unpack_state.canonical_memory_qwords.front() & 0x2u) != 0u;
  return owner_publication_clean && wrapped_unpack_clean;
}
#endif

bool VU_Thread::CanAdvanceGeneratedLoopKernelPrivateState(
	const VitaGpuVu::GeneratedLoopKernelTransaction& transaction,
	const std::vector<VitaGpuVu::VifUnpackSpan>& unpacks,
	GeneratedLoopKernelPrivateAdvanceProof* proof) const
{
	if (proof)
		*proof = {};
	if (transaction.UsesDeferredPairPlanStoreCommit() &&
		m_gpu_vu_generated_private_state.valid &&
		m_gpu_vu_generated_private_state.LiveDeferredMemoryOwnerCount() >=
			GeneratedLoopKernelPrivateState::DeferredMemoryOwnerCapacity)
	{
		return false;
	}
	if (transaction.DeferredSuccessor() &&
		m_gpu_vu_generated_private_state.valid &&
		!m_gpu_vu_generated_private_state.CanAssignDeferredRegisterOwner(
			transaction.DeferredSuccessor(), transaction.FinalVfLanes(),
			transaction.FinalAccLanes(), transaction.FinalQ(),
			transaction.FinalP(), transaction.FinalI()))
	{
		return false;
	}
	for (const VitaGpuVu::VifUnpackSpan& span : unpacks)
	{
		u32 payload_size = 0;
		const u8* const payload =
			VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!payload ||
			!VitaGpuVu::GetDirectAffineV4_32PayloadSize(
				span, &payload_size) ||
			payload_size != span.payload.size)
		{
			return false;
		}
	}

	const u64 store_words =
		static_cast<u64>(transaction.StoreEntryCount()) * 4u;
	if (store_words != transaction.OutputWordCount())
		return false;
	if (transaction.UsesGpuNativeOutputOnlyStoreCommit() ||
		transaction.UsesDeferredPairPlanStoreCommit())
	{
		if (!transaction.ExactStoreWords().empty() ||
			(transaction.UsesDeferredPairPlanStoreCommit() &&
			 !transaction.DeferredStores()))
			return false;
	}
	else if (transaction.ExactStoreWords().size() != store_words)
	{
		return false;
	}
	if (transaction.PreLoopStoreWords().size() !=
		static_cast<u64>(transaction.PreLoopStoreEntryCount()) * 4u)
	{
		return false;
	}
	if (proof)
	{
		proof->transaction = &transaction;
		// Bind the proof to the immutable span storage, not to its temporary
		// vector owner. AttachReplayUnpacks() transfers this same allocation
		// into the transaction before mailbox publication; changing the owner
		// object must not invalidate a pre-effect proof over identical bytes.
		proof->unpack_data = unpacks.data();
		proof->unpack_count = unpacks.size();
	}
	return true;
}

void VU_Thread::AdvanceGeneratedLoopKernelPrivateState(
	const VitaGpuVu::GeneratedLoopKernelTransaction& transaction,
	const std::vector<VitaGpuVu::VifUnpackSpan>& unpacks,
	const GeneratedLoopKernelPrivateAdvanceProof& proof,
	const VitaGpuVu::GeneratedLoopKernelFinalStateValues* resolved_successor)
{
	pxAssertRel(proof.Matches(transaction, unpacks),
		"generated GPU-VU private successor lost its pre-effect proof");
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	if (!state.valid)
	{
		std::memcpy(state.memory.data(), VU1.Mem, VU1_MEMSIZE);
		std::memcpy(state.vf.data(), &VU1.VF[0].UL[0],
			sizeof(state.vf));
		std::memcpy(state.acc.data(), &VU1.ACC.UL[0],
			sizeof(state.acc));
		for (u32 reg = 0; reg < state.vi.size(); reg++)
			state.vi[reg] = VU1.VI[reg].US[0];
		state.q = VU1.VI[REG_Q].UL;
		state.p = VU1.VI[REG_P].UL;
		state.i = VU1.VI[REG_I].UL;
		state.tpc = VU1.VI[REG_TPC].UL << 3;
		state.canonical_memory_qwords.fill(~0u);
		if (!state.raw_memory_provenance)
		{
			state.raw_memory_provenance =
				VitaGpuVu::PersistentVifMemoryProvenance::Create();
		}
		if (state.raw_memory_provenance)
			state.raw_memory_provenance->Clear();
		state.unavailable_memory_words.fill(0u);
		state.unavailable_memory_owner_sequence.fill(0u);
		state.unavailable_memory_owner_slot.fill(0u);
		state.native_memory_owners = {};
		state.deferred_memory_owners.clear();
		state.unavailable_vf_lanes.fill(0u);
		state.ResetDeferredRegisterOwners();
		state.unavailable_acc_lanes = 0u;
		state.q_available = true;
		state.p_available = true;
		state.i_available = true;
		state.valid = true;
	}

	const auto set_word_available = [&state](u32 word) {
		state.unavailable_memory_words[word >> 5] &=
			~(1u << (word & 31u));
		state.ClearUnavailableMemoryOwner(word);
		const u32 qword = word >> 2u;
		state.canonical_memory_qwords[qword >> 5] &=
			~(1u << (qword & 31u));
	};
	for (const VitaGpuVu::VifUnpackSpan& span : unpacks)
	{
		const bool retained = RetainOrMaterializeGpuVuPrivateUnpack(
			state.raw_memory_provenance.get(), span, state.memory.data());
		pxAssertRel(retained,
			"validated generated GPU-VU UNPACK lost its immutable input");
		state.ApplyCompleteQwordUnpackOwnership(
			span.destination_qword, span.vector_count);
	}

	const auto apply_known_stores = [&state, &set_word_available](
			const std::vector<VitaGpuVu::GeneratedLoopKernelStoreTarget>& targets,
			const std::vector<u32>& words) {
		for (u32 entry = 0; entry < targets.size(); entry++)
		{
			const auto& target = targets[entry];
			if (state.raw_memory_provenance)
			{
				pxAssertRel(state.raw_memory_provenance->
					MaterializeAndInvalidateQword(
						state.memory.data(), VU1_MEMSIZE, target.address_qword),
					"generated store lost its retained VIF source qword");
			}
			for (u32 lane = 0; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) == 0u)
					continue;
				const u32 word =
					static_cast<u32>(target.address_qword) * 4u + lane;
				state.memory[word] = words[entry * 4u + lane];
				set_word_available(word);
			}
		}
	};
	apply_known_stores(transaction.PreLoopStoreTargets(),
		transaction.PreLoopStoreWords());
	if (transaction.UsesGpuNativeOutputOnlyStoreCommit() ||
		transaction.UsesDeferredPairPlanStoreCommit())
	{
		u8 memory_owner_slot = 0u;
		bool replaced_full_deferred_owner = false;
		bool replaced_covered_deferred_owners = false;
		bool replaced_full_native_owner = false;
		bool replaced_covered_native_owners = false;
		if (transaction.UsesGpuNativeOutputOnlyStoreCommit())
		{
			memory_owner_slot = state.ReplaceFullyLiveNativeMemoryOwner(
				transaction.Sequence(), transaction.LayoutIdentity(),
				transaction.StoreWordCount());
			replaced_full_native_owner = memory_owner_slot != 0u;
			if (replaced_full_native_owner)
			{
				VitaGpuVu::
					RecordGeneratedLoopKernelPrivateSameLayoutReplacement();
			}
			else
			{
				u32 covered_owner_count = 0u;
				replaced_covered_native_owners =
					state.ReplaceCoveredNativeMemoryOwners(
						transaction.Sequence(), transaction.LayoutIdentity(),
						transaction.StoreWordCount(), transaction.StoreWordMasks(),
						&covered_owner_count);
				if (replaced_covered_native_owners)
				{
					VitaGpuVu::
						RecordGeneratedLoopKernelPrivateCoveredLayoutReplacement(
							covered_owner_count);
				}
				else
				{
					memory_owner_slot = state.RetainNativeMemoryOwner(
						transaction.Sequence(), transaction.LayoutIdentity(),
						transaction.StoreWordCount());
				}
			}
		}
		if (transaction.UsesDeferredPairPlanStoreCommit())
		{
			u32 covered_owner_count = 0u;
			replaced_covered_deferred_owners =
				state.ReplaceCoveredDeferredMemoryOwners(
					transaction.Sequence(), transaction.DeferredStores(),
					transaction.StoreWordMasks(), &covered_owner_count);
			if (replaced_covered_deferred_owners)
			{
				VitaGpuVu::
					RecordGeneratedLoopKernelPrivateCoveredLayoutReplacement(
						covered_owner_count);
			}
			else
			{
				memory_owner_slot = state.ReplaceFullyLiveDeferredMemoryOwner(
					transaction.Sequence(), transaction.DeferredStores(),
					transaction.LayoutIdentity(), transaction.StoreWordCount());
				replaced_full_deferred_owner = memory_owner_slot != 0u;
				if (replaced_full_deferred_owner)
				{
					VitaGpuVu::
						RecordGeneratedLoopKernelPrivateSameLayoutReplacement();
				}
				else
				{
					memory_owner_slot = state.RetainDeferredMemoryOwner(
						transaction.Sequence(), transaction.DeferredStores(),
						transaction.LayoutIdentity(), transaction.StoreWordCount());
				}
				pxAssertRel(memory_owner_slot != 0u,
					"generated GPU-VU deferred memory-owner pool is full");
			}
		}
		if (state.raw_memory_provenance)
		{
			pxAssertRel(state.raw_memory_provenance->
				InvalidateFullyOverwrittenQwords(
					transaction.CompleteWriteQwordMasks().data(),
					static_cast<u32>(
						transaction.CompleteWriteQwordMasks().size())),
				"generated full-qword stores lost their retained VIF owners");
			for (u32 mask_word = 0u;
				mask_word < transaction.StoreQwordMasks().size(); mask_word++)
			{
				u32 partial = transaction.StoreQwordMasks()[mask_word] &
					~transaction.CompleteWriteQwordMasks()[mask_word];
				while (partial != 0u)
				{
					const u32 bit = static_cast<u32>(__builtin_ctz(partial));
					pxAssertRel(state.raw_memory_provenance->
						MaterializeAndInvalidateQword(
							state.memory.data(), VU1_MEMSIZE,
							static_cast<u16>(mask_word * 32u + bit)),
						"partial generated store lost its retained VIF source qword");
					partial &= partial - 1u;
				}
			}
		}
		if (!replaced_full_deferred_owner &&
			!replaced_covered_deferred_owners &&
			!replaced_full_native_owner &&
			!replaced_covered_native_owners)
		{
			state.SetUnavailableMemoryOwnerMask(
				transaction.StoreWordMasks(), transaction.Sequence(),
				memory_owner_slot);
		}
		for (u32 mask_word = 0u;
			mask_word < transaction.StoreQwordMasks().size(); mask_word++)
		{
			state.canonical_memory_qwords[mask_word] &=
				~transaction.StoreQwordMasks()[mask_word];
		}
	}
	else
	{
		apply_known_stores(
			transaction.StoreTargets(), transaction.ExactStoreWords());
	}
	for (u16 qword : transaction.AdcPatchQwords())
	{
		if (state.raw_memory_provenance)
		{
			pxAssertRel(state.raw_memory_provenance->
				MaterializeAndInvalidateQword(
					state.memory.data(), VU1_MEMSIZE, qword),
				"generated ADC patch lost its retained VIF source qword");
		}
		const u32 word = static_cast<u32>(qword) * 4u + 3u;
		state.memory[word] = 0x00008000u;
		set_word_available(word);
	}
	state.ReleaseDeadDeferredMemoryOwners();
	const auto& vf_lanes = transaction.FinalVfLanes();
	const auto& deferred = transaction.DeferredSuccessor();
	if (deferred && !resolved_successor)
	{
		bool register_owner_replaced = false;
		u32 register_owner_replacement_slots = 0u;
		const bool owner_assigned = state.AssignDeferredRegisterOwner(
			deferred, vf_lanes, transaction.FinalAccLanes(),
			transaction.FinalQ(), transaction.FinalP(), transaction.FinalI(),
			&register_owner_replaced, &register_owner_replacement_slots);
		pxAssertRel(owner_assigned,
			"preflighted generated GPU-VU register-owner pool is full");
		if (!owner_assigned)
			return;
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			VitaGpuVu::RecordGeneratedLoopKernelPrivateRegisterOwnerUpdate(
				register_owner_replaced, register_owner_replacement_slots);
		}
		for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				const u8 bit = static_cast<u8>(0x8u >> lane);
				if ((vf_lanes[reg] & bit) == 0u)
					continue;
				state.unavailable_vf_lanes[reg] |= bit;
			}
		}
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u8 bit = static_cast<u8>(0x8u >> lane);
			if ((transaction.FinalAccLanes() & bit) == 0u)
				continue;
			state.unavailable_acc_lanes |= bit;
		}
		if (transaction.FinalQ())
		{
			state.q_available = false;
		}
		if (transaction.FinalP())
		{
			state.p_available = false;
		}
		if (transaction.FinalI())
		{
			state.i_available = false;
		}
	}
	else
	{
		const auto& final_vf = resolved_successor ?
			resolved_successor->vf : transaction.FinalVfValues();
		for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				const u8 bit = static_cast<u8>(0x8u >> lane);
				if ((vf_lanes[reg] & bit) == 0u)
					continue;
				state.vf[reg][lane] = final_vf[reg][lane];
				state.unavailable_vf_lanes[reg] &= ~bit;
				state.ClearDeferredRegisterOwnerSlot(
					&state.deferred_vf_owner_slots[reg * 4u + lane]);
			}
		}
		const auto& final_acc = resolved_successor ?
			resolved_successor->acc : transaction.FinalAccValues();
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u8 bit = static_cast<u8>(0x8u >> lane);
			if ((transaction.FinalAccLanes() & bit) == 0u)
				continue;
			state.acc[lane] = final_acc[lane];
			state.unavailable_acc_lanes &= ~bit;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_acc_owner_slots[lane]);
		}
		if (transaction.FinalQ())
		{
			state.q = resolved_successor ?
				resolved_successor->q : transaction.FinalQValue();
			state.q_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_q_owner_slot);
		}
		if (transaction.FinalP())
		{
			state.p = resolved_successor ?
				resolved_successor->p : transaction.FinalPValue();
			state.p_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_p_owner_slot);
		}
		if (transaction.FinalI())
		{
			state.i = resolved_successor ?
				resolved_successor->i : transaction.FinalIValue();
			state.i_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_i_owner_slot);
		}
	}
	const auto& final_vi = transaction.FinalViValues();
	for (u32 reg = 1u; reg < final_vi.size(); reg++)
	{
		if ((transaction.FinalViWriteMask() & (1u << reg)) != 0u)
			state.vi[reg] = final_vi[reg];
	}
	state.vi[0] = 0u;
	state.tpc = transaction.UniqueResumePc();
}

bool VU_Thread::MaterializeGeneratedLoopKernelDeferredMemory()
{
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	if (!state.valid)
		return false;

	for (u32 owner_index = 0u;
		owner_index < state.deferred_memory_owners.size(); owner_index++)
	{
		const auto& retained = state.deferred_memory_owners[owner_index];
		if (retained.live_word_count == 0u)
			continue;
		if (retained.sequence == 0u || !retained.owner)
			return false;

		auto& words = state.deferred_store_words;
		size_t word_count = 0u;
		std::string error;
		if (!retained.owner->Evaluate(words.data(), words.size(),
				&word_count, GetGeneratedLoopKernelEvaluationWorkspace(), &error))
		{
			Console.Error(
				"GPU-VU: deferred exact store materialization failed: %s.",
				error.empty() ? "unknown store error" : error.c_str());
			return false;
		}
		const auto& targets = retained.owner->Targets();
		if (targets.empty() || word_count != targets.size() * 4u)
			return false;
		for (u32 entry = 0u; entry < targets.size(); entry++)
		{
			const auto& target = targets[entry];
			if (target.address_qword >= 1024u || target.lane_mask == 0u ||
				(target.lane_mask & ~0x0fu) != 0u)
			{
				return false;
			}
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) == 0u)
					continue;
				const u32 word =
					static_cast<u32>(target.address_qword) * 4u + lane;
				if (state.unavailable_memory_owner_slot[word] !=
					static_cast<u8>(owner_index + 1u))
				{
					continue;
				}
                                state.memory[word] = words[entry * 4u + lane];
                                state.unavailable_memory_words[word >> 5] &=
                                    ~(1u << (word & 31u));
                                state.ClearUnavailableMemoryOwner(word);
                        }
                }
        }

        // Native-SGX output-only lanes are copied into the private image by
        // vertex retirement but deliberately keep their compact ownership
        // partition while generated execution continues. Publish that
        // already-materialized image only at this real observer. This preserves
        // the old canonical result while avoiding a lane scatter/clear/reassign
        // cycle for every hot Execute.
        if (!state.PublishMaterializedNativeMemoryOwners())
          return false;
        state.deferred_memory_owners.clear();
        return std::all_of(state.unavailable_memory_words.begin(),
                           state.unavailable_memory_words.end(),
                           [](u32 word) { return word == 0u; });
}

bool VU_Thread::MaterializeGeneratedLoopKernelDeferredState()
{
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	if (!state.valid)
		return false;

	std::array<std::shared_ptr<const
		VitaGpuVu::GeneratedLoopKernelDeferredSuccessor>, 16u> owners{};
	size_t owner_count = 0u;
	const auto append_owner = [&owners, &owner_count](const auto& owner) {
		if (!owner)
			return false;
		if (std::none_of(owners.begin(), owners.begin() + owner_count,
				[&owner](const auto& prior) {
					return prior.get() == owner.get();
				}))
		{
			if (owner_count == owners.size())
				return false;
			owners[owner_count++] = owner;
		}
		return true;
	};
	for (const auto& retained : state.deferred_register_owners)
	{
		if (retained.live_reference_count != 0u &&
			!append_owner(retained.owner))
		{
			return false;
		}
	}

	for (size_t owner_index = 0u; owner_index < owner_count; owner_index++)
	{
		const auto& owner = owners[owner_index];
		VitaGpuVu::GeneratedLoopKernelFinalStateValues values;
		std::string error;
		m_gpu_vu_generated_deferred_successor_evaluations++;
		if (!owner->Evaluate(&values,
				GetGeneratedLoopKernelEvaluationWorkspace(), &error))
		{
			Console.Error(
				"GPU-VU: deferred exact successor materialization failed: %s.",
				error.empty() ? "unknown successor error" : error.c_str());
			return false;
		}
		for (u32 reg = 1u; reg < state.unavailable_vf_lanes.size(); reg++)
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				const u8 bit = static_cast<u8>(0x8u >> lane);
				u8& lane_owner_slot =
					state.deferred_vf_owner_slots[reg * 4u + lane];
				if ((state.unavailable_vf_lanes[reg] & bit) == 0u ||
					state.DeferredRegisterOwnerForSlot(
						lane_owner_slot).get() != owner.get())
				{
					continue;
				}
				state.vf[reg][lane] = values.vf[reg][lane];
				state.unavailable_vf_lanes[reg] &= ~bit;
				state.ClearDeferredRegisterOwnerSlot(&lane_owner_slot);
			}
		}
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u8 bit = static_cast<u8>(0x8u >> lane);
			u8& lane_owner_slot = state.deferred_acc_owner_slots[lane];
			if ((state.unavailable_acc_lanes & bit) == 0u ||
				state.DeferredRegisterOwnerForSlot(
					lane_owner_slot).get() != owner.get())
			{
				continue;
			}
			state.acc[lane] = values.acc[lane];
			state.unavailable_acc_lanes &= ~bit;
			state.ClearDeferredRegisterOwnerSlot(&lane_owner_slot);
		}
		if (!state.q_available &&
			state.DeferredRegisterOwnerForSlot(
				state.deferred_q_owner_slot).get() == owner.get())
		{
			state.q = values.q;
			state.q_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_q_owner_slot);
		}
		if (!state.p_available &&
			state.DeferredRegisterOwnerForSlot(
				state.deferred_p_owner_slot).get() == owner.get())
		{
			state.p = values.p;
			state.p_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_p_owner_slot);
		}
		if (!state.i_available &&
			state.DeferredRegisterOwnerForSlot(
				state.deferred_i_owner_slot).get() == owner.get())
		{
			state.i = values.i;
			state.i_available = true;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_i_owner_slot);
		}
	}

	return std::all_of(state.unavailable_vf_lanes.begin(),
			state.unavailable_vf_lanes.end(), [](u8 lanes) {
				return (lanes & 0x0fu) == 0u;
			}) &&
		(state.unavailable_acc_lanes & 0x0fu) == 0u &&
		state.q_available && state.p_available && state.i_available;
}

void VU_Thread::RecordGeneratedLoopKernelPrivateCompletion(u32 cycle)
{
	const u64 completion = m_gpu_vu_generated_private_completion_count++;
	m_gpu_vu_generated_private_completion_cycles[completion & 3u] = cycle;
}

bool VU_Thread::RetireCompletedGeneratedLoopKernelTransactions()
{
	GeneratedRetirementTelemetryScope telemetry_scope;
	const auto fail_retirement = [](const char* reason,
		const std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>&
			transaction) {
		const u64 sequence = transaction ? transaction->Sequence() : 0u;
		const VitaGpuVu::GeneratedLoopKernelTransactionStage stage =
			transaction ? transaction->Stage() :
				VitaGpuVu::GeneratedLoopKernelTransactionStage::Failed;
		const VitaGpuVu::GeneratedLoopKernelTransactionFailure failure =
			transaction ? transaction->Failure() :
				VitaGpuVu::GeneratedLoopKernelTransactionFailure::Unspecified;
		const char* const detail = transaction && transaction->FailureDetail() ?
			transaction->FailureDetail() : "none";
		Console.Error(
			"GPU-VU seq=%llu private_retirement=FAILED reason=%s stage=%s "
			"failure=%s detail=%s.",
			static_cast<unsigned long long>(sequence), reason ? reason : "unknown",
			VitaGpuVu::GeneratedLoopKernelTransactionStageName(stage),
			VitaGpuVu::GeneratedLoopKernelTransactionFailureName(failure), detail);
#if defined(__vita__)
		// A following fail-closed assertion terminates the process immediately.
		// Mirror this exceptional record to the coredump TTY stream so the exact
		// ownership invariant survives even when the normal file logger has not
		// yet flushed its final line.
		std::fprintf(stderr,
			"GPU-VU seq=%llu private_retirement=FAILED reason=%s stage=%s "
			"failure=%s detail=%s\n",
			static_cast<unsigned long long>(sequence), reason ? reason : "unknown",
			VitaGpuVu::GeneratedLoopKernelTransactionStageName(stage),
			VitaGpuVu::GeneratedLoopKernelTransactionFailureName(failure), detail);
		std::fflush(stderr);
#endif
		return false;
	};
	while (!m_gpu_vu_generated_pending_transactions.empty())
	{
		const std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>&
			transaction = m_gpu_vu_generated_pending_transactions.front();
		if (!transaction)
			return fail_retirement("null-transaction", transaction);
		const auto stage = transaction->Stage();
		if (stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::Prepared ||
			stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::GsAccepted)
			break;
		if (stage !=
				VitaGpuVu::GeneratedLoopKernelTransactionStage::GpuCompleted)
		{
			return fail_retirement("non-completed-stage", transaction);
		}

		const auto& targets = transaction->StoreTargets();
		const auto& committed_words = transaction->CommittedStoreWords();
		if (targets.empty() ||
			(transaction->UsesDeferredPairPlanStoreCommit()
				? !transaction->OutputWords().empty()
				: transaction->OutputWords().size() !=
					transaction->OutputWordCount()) ||
			(transaction->UsesDeferredPairPlanStoreCommit()
				? !committed_words.empty()
				: committed_words.size() != transaction->OutputWordCount()))
		{
			Console.Error(
				"GPU-VU seq=%llu private_retirement_shape targets=%u output=%u/%u "
				"committed=%u mode=%s.",
				static_cast<unsigned long long>(transaction->Sequence()),
				static_cast<u32>(targets.size()),
				static_cast<u32>(transaction->OutputWords().size()),
				transaction->OutputWordCount(),
				static_cast<u32>(committed_words.size()),
				transaction->StoreCommitModeName());
			return fail_retirement("successor-payload-shape", transaction);
		}
		GeneratedLoopKernelPrivateState& state =
			m_gpu_vu_generated_private_state;
		if (!state.valid)
			return fail_retirement("private-state-not-valid", transaction);
                if (transaction->UsesGpuNativeOutputOnlyStoreCommit()) {
                  const u32 live_native_words =
                      state.LiveNativeMemoryOwnerWordCount(
                          transaction->Sequence());
                  const u32 live_fallback_words =
                      state.LiveFallbackMemoryOwnerWordCount(
                          transaction->Sequence());
                  const u32 live_words =
                      live_native_words + live_fallback_words;
                  u32 copied_words = 0u;
                  if (live_words != 0u) {
                    for (u32 entry = 0u; entry < targets.size(); entry++) {
                      const auto &target = targets[entry];
                      for (u32 lane = 0u; lane < 4u; lane++) {
                        if ((target.lane_mask & (0x8u >> lane)) == 0u)
                          continue;
                        const u32 word =
                            static_cast<u32>(target.address_qword) * 4u + lane;
                        if (state.UnavailableMemoryOwnerSequence(word) !=
                            transaction->Sequence()) {
                          continue;
                        }
                        state.memory[word] = committed_words[entry * 4u + lane];
                        if (state.unavailable_memory_owner_slot[word] == 0u) {
                          state.unavailable_memory_words[word >> 5u] &=
                              ~(1u << (word & 31u));
                          state.unavailable_memory_owner_sequence[word] = 0u;
                        }
                        copied_words++;
                      }
                    }
                    if (copied_words != live_words ||
                        (live_native_words != 0u &&
                         !state.MarkNativeMemoryOwnerMaterialized(
                             transaction->Sequence()))) {
                      return fail_retirement(
                          "native-owner-materialization-shape", transaction);
                    }
                  }
                }

                std::vector<VitaGpuVu::VifUnpackSpan> replay_unpacks =
			transaction->TakeReplayUnpacks();
		for (VitaGpuVu::VifUnpackSpan& span : replay_unpacks)
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
		if (!transaction->MarkAdopted())
			return fail_retirement("mark-adopted-race", transaction);

		const u64 sequence = transaction->Sequence();
		const u32 pairs = transaction->ExecutedPairs();
		RecordGeneratedLoopKernelPrivateCompletion(4u);
		m_gpu_vu_generated_private_retired_transactions++;
		m_gpu_vu_generated_private_retired_pairs += pairs;
		VitaGpuVu::RecordGeneratedLoopKernelAccepted(pairs);
		VitaGpuVu::RecordUniversalGpuVuProviderCompletion(true, 0u, 0u);
		if (sequence <= 8u || (sequence & (sequence - 1u)) == 0u)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-loop-kernel accepted=1 "
				"product_accepted=1 generated_epochs=1 pairs=%u "
				"output=direct-tfx output_profile=%s state=transactional "
				"transaction_commit=private-retired canonical_publication=deferred "
				"cpu_vu_calls=0 cpu_vu_pairs=0.",
				static_cast<unsigned long long>(sequence), pairs,
				VitaGpuVu::GeneratedLoopKernelNumericProfileName(
					transaction->OutputNumericProfile()));
		}
		m_gpu_vu_generated_pending_transactions.pop_front();

		while (!m_gpu_vu_generated_private_continuations.empty() &&
			m_gpu_vu_generated_private_continuations.front().predecessor_sequence ==
				sequence)
		{
			GeneratedPrivateStateLoadContinuation& continuation =
				m_gpu_vu_generated_private_continuations.front();
			for (VitaGpuVu::VifUnpackSpan& span : continuation.replay_unpacks)
				VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continuation.replay_unpacks.clear();
			RecordGeneratedLoopKernelPrivateCompletion(continuation.final_cycle);
			++m_gpu_vu_private_state_load_adopted_count;
			m_gpu_vu_generated_private_continuations.pop_front();
		}
	}
	PublishGeneratedLoopKernelPendingExecutionCount();
	return true;
}

bool VU_Thread::CommitGeneratedLoopKernelPrivateState()
{
	GeneratedCommitTelemetryScope telemetry_scope;
	if (!m_gpu_vu_generated_pending_transactions.empty() ||
		!m_gpu_vu_generated_private_continuations.empty())
	{
		return false;
	}
	if (!m_gpu_vu_generated_private_state.valid)
		return m_gpu_vu_generated_private_completion_count == 0u;
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	if (!MaterializeGeneratedLoopKernelDeferredMemory() ||
		std::any_of(state.unavailable_memory_words.begin(),
			state.unavailable_memory_words.end(),
			[](u32 word) { return word != 0u; }) ||
		!MaterializeGeneratedLoopKernelDeferredState() ||
		(state.raw_memory_provenance &&
		 !state.raw_memory_provenance->MaterializeOwnedQwords(
			 state.memory.data(), VU1_MEMSIZE)))
	{
		return false;
	}

	std::memcpy(VU1.Mem, state.memory.data(), VU1_MEMSIZE);
	std::memcpy(&VU1.VF[0].UL[0], &state.vf[0][0], sizeof(state.vf));
	std::memcpy(&VU1.ACC.UL[0], state.acc.data(), sizeof(state.acc));
	for (u32 reg = 0u; reg < state.vi.size(); reg++)
		VU1.VI[reg].US[0] = state.vi[reg];
	VU1.VI[0].UL = 0u;
	VU1.VI[REG_Q].UL = state.q;
	VU1.VI[REG_P].UL = state.p;
	VU1.VI[REG_I].UL = state.i;
	VU1.VI[REG_TPC].UL = state.tpc >> 3;

	const u64 completions = m_gpu_vu_generated_private_completion_count;
	if (completions != 0u)
	{
		BeginProgram();
		EndProgram(InterruptFlagVUEBit);
		const u64 retained = std::min<u64>(completions, 4u);
		for (u64 offset = completions - retained; offset < completions; offset++)
		{
			vuCycles[(vuCycleIdx + static_cast<u32>(offset)) & 3u].store(
				m_gpu_vu_generated_private_completion_cycles[offset & 3u],
				std::memory_order_release);
		}
		vuCycleIdx = (vuCycleIdx + static_cast<u32>(completions)) & 3u;
	}
	// CPU fallback currently makes this an architectural observer for every
	// unsupported sibling Execute.  Logging every observer added roughly one
	// hundred formatted console writes per second to BSpline's CPU1 hot path.
	// Preserve unmistakable evidence without making diagnostics part of the
	// execution cost; cumulative telemetry owns the unsampled count.
	static u64 private_generation_commit_reports = 0u;
	const u64 commit_report = ++private_generation_commit_reports;
	if (commit_report <= 8u ||
		(commit_report & (commit_report - 1u)) == 0u)
	{
		Console.WriteLn(
			"GPU-VU generated_private_generation=committed report=%llu "
			"transactions=%llu logical_completions=%llu pairs=%llu "
			"canonical_copies=1 deferred_successors=materialized "
			"deferred_successor_evaluations_total=%llu "
			"transaction_commit=adopted.",
			static_cast<unsigned long long>(commit_report),
			static_cast<unsigned long long>(
				m_gpu_vu_generated_private_retired_transactions),
			static_cast<unsigned long long>(completions),
			static_cast<unsigned long long>(
				m_gpu_vu_generated_private_retired_pairs),
			static_cast<unsigned long long>(
				m_gpu_vu_generated_deferred_successor_evaluations));
	}
	m_gpu_vu_generated_private_tail_sequence = 0u;
	m_gpu_vu_generated_private_retired_transactions = 0u;
	m_gpu_vu_generated_private_retired_pairs = 0u;
	m_gpu_vu_generated_private_completion_count = 0u;
	m_gpu_vu_generated_private_completion_cycles.fill(0u);
	InvalidateGeneratedLoopKernelPrivateState();
	PublishGeneratedLoopKernelPendingExecutionCount();
	return true;
}

u32 VU_Thread::GeneratedLoopKernelPendingExecutionCount() const
{
	return static_cast<u32>(
		m_gpu_vu_generated_pending_transactions.size() +
		m_gpu_vu_generated_private_continuations.size() +
		(m_gpu_vu_generated_private_completion_count != 0u ? 1u : 0u));
}

u32 VU_Thread::GeneratedLoopKernelPhysicalTransactionCount() const
{
	return static_cast<u32>(m_gpu_vu_generated_pending_transactions.size());
}

void VU_Thread::PublishGeneratedLoopKernelPendingExecutionCount()
{
	m_gpu_vu_generated_pending_transaction_count.store(
		GeneratedLoopKernelPendingExecutionCount(),
		std::memory_order_release);
}

bool VU_Thread::TryQueueGeneratedLoopKernelProductHotExecute(
	UniversalDispatchCostCacheEntry& cache, u32 entry_pc,
	u16 vif_top, u16 vif_itop,
	std::vector<VitaGpuVu::VifUnpackSpan>* unpacks,
	GeneratedProductHotQueueResult* result, std::string* error)
{
	const Common::Timer::Value hot_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	if (result)
		*result = {};
	if (error)
		error->clear();
	if (!unpacks || !cache.valid || !cache.generated_product_ready ||
		GeneratedLoopKernelPhysicalTransactionCount() >=
			MaximumGeneratedLoopKernelInFlightExecutions ||
		cache.micro_generation != m_gpu_vu_micro_generation ||
		cache.entry_pc != entry_pc || cache.vif_top != vif_top ||
		cache.vif_itop != vif_itop || cache.generated_program_identity == 0u ||
		(cache.generated_key_low == 0u && cache.generated_key_high == 0u) ||
		cache.dynamic_pair_upper_bound == 0u)
	{
		return false;
	}

	const GeneratedLoopKernelPrivateState* const private_state =
		m_gpu_vu_generated_private_state.valid ?
			&m_gpu_vu_generated_private_state : nullptr;
	std::array<u16, 16> initial_vi{};
	if (private_state)
	{
		initial_vi = private_state->vi;
	}
	else
	{
		for (u32 reg = 0u; reg < initial_vi.size(); reg++)
			initial_vi[reg] = VU1.VI[reg].US[0];
	}

	static thread_local GpuVuMemoryView::BulkWorkspace memory_workspace;
	memory_workspace.Begin();
		GpuVuMemoryView memory_view{
		private_state ? reinterpret_cast<const u8*>(
			private_state->memory.data()) : VU1.Mem,
		unpacks,
		private_state ? private_state->unavailable_memory_words.data() : nullptr,
		&memory_workspace, VU1.Mem,
			private_state ? private_state->canonical_memory_qwords.data() : nullptr,
			private_state && private_state->raw_memory_provenance ?
				private_state->raw_memory_provenance.get() : nullptr};
	VitaGpuVu::InvocationEvaluationContext context;
	context.vif_top = vif_top;
	context.vif_itop = vif_itop;
	context.initial_vi = initial_vi.data();
	context.initial_vf_words = private_state ?
		&private_state->vf[0][0] : &VU1.VF[0].UL[0];
	context.initial_acc_words = private_state ?
		private_state->acc.data() : &VU1.ACC.UL[0];
	context.initial_q = private_state ? private_state->q : VU1.VI[REG_Q].UL;
	context.initial_p = private_state ? private_state->p : VU1.VI[REG_P].UL;
	context.initial_i = private_state ? private_state->i : VU1.VI[REG_I].UL;
	context.unavailable_initial_vf_lanes = private_state ?
		private_state->unavailable_vf_lanes.data() : nullptr;
	context.unavailable_initial_acc_lanes = private_state ?
		private_state->unavailable_acc_lanes : 0u;
	context.initial_q_available = !private_state || private_state->q_available;
	context.initial_p_available = !private_state || private_state->p_available;
	context.initial_i_available = !private_state || private_state->i_available;
	context.memory_user = &memory_view;
	context.read_memory_u16 = ReadGpuVuMemoryU16;
	context.read_memory_u32 = ReadGpuVuMemoryU32;
	context.read_memory_qwords = ReadGpuVuMemoryQwords;
	context.memory_qwords_have_canonical_owner =
		GpuVuMemoryQwordsHaveCanonicalOwner;
	context.bind_memory_qwords_to_raw_payload = BindGpuVuMemoryRawQwords;

	VitaGpuVu::GeneratedLoopKernelLiveContract live_contract;
	const VitaGpuVu::ShaderKey preferred_executable_key{
		cache.generated_key_low, cache.generated_key_high};
	const Common::Timer::Value resolution_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	if (!VitaGpuVu::ResolveGeneratedLoopKernelLiveContract(
			cache.generated_program_identity, entry_pc,
			cache.configuration_bits, preferred_executable_key,
			context, &live_contract, error))
	{
		VitaGpuVu::RecordGeneratedLoopKernelProductHotDispatch(false);
		if (resolution_started != 0)
		{
			VitaGpuVu::RecordGeneratedLoopKernelLiveContractResolution(
				MtvuElapsedTelemetryMicroseconds(resolution_started));
		}
		return false;
	}
	VitaGpuVu::RecordGeneratedLoopKernelProductHotDispatch(true);
	if (resolution_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelLiveContractResolution(
			MtvuElapsedTelemetryMicroseconds(resolution_started));
	}

	VitaGpuVu::GeneratedLoopKernelBundle bundle = live_contract.descriptor;
	const VitaGpuVu::GeneratedProgramState generated_state =
		VitaGpuVu::QueryGeneratedProgram(bundle.kernel_key);
	const bool compiler_owned = bundle.MatchesOwner(
		cache.generated_program_identity, entry_pc,
		cache.configuration_bits, bundle.gif_tag);
	const bool transactional_successor =
		!bundle.requires_transactional_final_state &&
		bundle.uses_closed_form_nested_loop &&
		bundle.has_compact_final_state_formula &&
		bundle.private_store_count != 0u && live_contract.HasPreEffectProof();
	const VitaGpuVu::GeneratedLoopKernelAttestationIdentity
		attestation_identity{
			bundle.attestation_key, VitaGpuVu::GeneratedProgramAbiVersion,
			bundle.abi_version, bundle.configuration_bits,
			bundle.semantic_profile_key};
	const VitaGpuVu::GeneratedLoopKernelAttestationRecord attestation =
		VitaGpuVu::QueryGeneratedLoopKernelAttestation(attestation_identity);
	if (!VitaGpuVu::GeneratedLoopKernelProductExecutionIsReady(
			generated_state, bundle.IsNoWriteProduct(), compiler_owned, transactional_successor,
			VitaGpuVu::GeneratedProgramHasProductResourceAttestation(
				bundle.kernel_key),
			attestation.state))
	{
		cache.generated_product_ready = false;
		if (error && error->empty())
			*error = "generated product attestation is no longer ready";
		return false;
	}

	const Common::Timer::Value descriptor_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	auto draw = VitaGpuVu::BuildGeneratedLoopKernelGpuVuDraw(
		std::move(live_contract), context, *unpacks,
		cache.dynamic_pair_upper_bound, attestation.numeric_profile, error);
	if (descriptor_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelDescriptorBuild(
			MtvuElapsedTelemetryMicroseconds(descriptor_started));
	}
	if (!draw)
		return false;

	const u32 vertices = draw->vertex_count;
	const u32 primitives = draw->primitive_count;
	const u32 executed_pairs = draw->executed_pair_count;
	const u32 resume_pc = draw->unique_resume_pc;
	const auto transaction = draw->GeneratedLoopKernelTransactionOwner();
	GeneratedLoopKernelPrivateAdvanceProof private_advance_proof;
	if (!transaction ||
		!CanAdvanceGeneratedLoopKernelPrivateState(
			*transaction, *unpacks, &private_advance_proof))
	{
		if (error && error->empty())
			*error = "generated product has no private successor";
		return false;
	}
	const u64 sequence = VitaGpuVu::NextGpuVuOrderingSequence();
	const Common::Timer::Value queue_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	const bool sealed =
		draw->SealGeneratedLoopKernelProductForQueue(sequence, error);
	if (!sealed)
		return false;
	const bool attached = transaction->AttachReplayUnpacks(unpacks);
	if (!attached)
	{
		transaction->MarkFailed(
			VitaGpuVu::GeneratedLoopKernelTransactionFailure::
				CpuJournalAttachment,
			"attach replay journal before generated mailbox publication");
		if (error && error->empty())
			*error = "generated product replay journal attachment failed";
		return false;
	}
	if (!private_advance_proof.Matches(
			*transaction, transaction->ReplayUnpacks()))
	{
		const bool recovered =
			transaction->RecoverReplayUnpacksBeforeGsAcceptance(unpacks);
		pxAssertRel(recovered,
			"generated transaction could not recover a moved proof journal");
		transaction->MarkFailed(
			VitaGpuVu::GeneratedLoopKernelTransactionFailure::
				CpuJournalAttachment,
			"replay journal storage changed while binding pre-effect proof");
		if (error && error->empty())
			*error = "generated product replay journal changed proof storage";
		return false;
	}
	const bool queued = VitaGS::QueueGpuVuDraw(std::move(draw));
	if (queue_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelQueue(
			MtvuElapsedTelemetryMicroseconds(queue_started));
	}
	if (!queued)
	{
		const bool recovered =
			transaction->RecoverReplayUnpacksBeforeGsAcceptance(unpacks);
		pxAssertRel(recovered,
			"rejected generated GPU-VU draw lost its replay journal");
		if (error && error->empty())
		{
			*error = recovered ?
				"direct draw mailbox rejected the generated product" :
				"direct draw mailbox rejected the generated product after GXM acceptance";
		}
		return false;
	}

	cache.allow_multi_execute_gather = true;
	cache.generated_product_ready = true;
	m_gpu_vu_generated_generation_had_product = true;
	cache.generated_key_low = bundle.kernel_key.low;
	cache.generated_key_high = bundle.kernel_key.high;
	cache.generated_attestation_key_low = bundle.attestation_key.low;
	cache.generated_attestation_key_high = bundle.attestation_key.high;
	cache.generated_loop_kernel_abi = bundle.abi_version;
	cache.generated_semantic_profile_key = bundle.semantic_profile_key;
	const Common::Timer::Value advance_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	// The generated GXP owns this execution. Keep its exact successor formula
	// dormant in the private generation instead of evaluating the same VU
	// arithmetic on CPU1 before every draw. A following generated root may
	// proceed when it does not consume those lanes; a genuine architectural
	// observer materializes only the surviving lane owners.
	AdvanceGeneratedLoopKernelPrivateState(
			*transaction, transaction->ReplayUnpacks(),
			private_advance_proof, nullptr);
	if (transaction->HasDeferredSuccessor())
	{
		pxAssertRel(transaction->ReleaseTransferredDeferredSuccessor(),
			"generated product retained a transferred exact successor graph");
	}
	if (transaction->UsesDeferredPairPlanStoreCommit())
	{
		pxAssertRel(transaction->ReleaseTransferredDeferredStores(),
			"generated product retained a transferred exact store graph");
	}
	if (advance_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelPrivateStateAdvance(
			MtvuElapsedTelemetryMicroseconds(advance_started));
	}
	m_deferred_vif_unpack_count.store(0u, std::memory_order_release);
	m_gpu_vu_generated_pending_transactions.push_back(transaction);
	m_gpu_vu_generated_private_tail_sequence = sequence;
	PublishGeneratedLoopKernelPendingExecutionCount();
	m_execute_jobs_completed.fetch_add(1u, std::memory_order_release);
	m_ring_space_progress.NotifyOfProgress();
	VitaGpuVu::RecordUniversalGpuVuAsyncQueued(
		GeneratedLoopKernelPendingExecutionCount());

	if (result)
	{
		result->sequence = sequence;
		result->executed_pairs = executed_pairs;
		result->vertices = vertices;
		result->primitives = primitives;
		result->resume_pc = resume_pc;
	}
	if (hot_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelHotExecute(
			MtvuElapsedTelemetryMicroseconds(hot_started));
	}
	return true;
}

bool VU_Thread::TryQueueAttestedGeneratedProductHotChain(
	UniversalDispatchCostCacheEntry& base_cache, u32 base_entry_pc,
	bool base_resume, u16 base_vif_top, u16 base_vif_itop,
	std::vector<VitaGpuVu::VifUnpackSpan>* base_unpacks,
	std::vector<BufferedGeneratedExecute>* buffered_executes)
{
	if (!base_unpacks || !buffered_executes || buffered_executes->empty() ||
		(!base_cache.generated_product_ready &&
		 !(m_gpu_vu_generated_private_state.valid &&
		   base_cache.generated_state_formula_ready)))
	{
		return false;
	}

	const Common::Timer::Value chain_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
	Pcsx2Trace::NotifyMachineCheckpointVu1ProgramStarted();
#endif

	GeneratedProductHotQueueResult base_result{};
	std::string hot_error;
	u64 base_formula_sequence = 0u;
	bool base_is_formula = false;
	bool base_queued = false;
	if (m_gpu_vu_generated_private_state.valid &&
		base_cache.generated_state_formula_ready)
	{
		base_queued = TryQueueGeneratedStateLoadFormulaContinuation(
			base_cache.generated_state_formula, base_resume,
			&base_formula_sequence);
		base_is_formula = base_queued;
	}
	else if (base_cache.generated_product_ready)
	{
		base_queued = TryQueueGeneratedLoopKernelProductHotExecute(
			base_cache, base_entry_pc, base_vif_top, base_vif_itop,
			base_unpacks, &base_result, &hot_error);
	}
	if (!base_queued)
		return false;

	u32 chain_executes = 1u;
	u32 chain_draws = base_is_formula ? 0u : 1u;
	u32 chain_formulas = base_is_formula ? 1u : 0u;
	u64 chain_pairs = base_is_formula ?
		base_cache.generated_state_formula.executed_pairs :
		base_result.executed_pairs;
	u64 first_sequence = base_is_formula ?
		base_formula_sequence : base_result.sequence;
	u64 last_sequence = first_sequence;
	s32 consumed_end = m_read_pos;
	const u32 configuration_bits =
		VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits();

	for (BufferedGeneratedExecute& buffered : *buffered_executes)
	{
		if (!m_gpu_vu_generated_private_state.valid)
		{
			break;
		}

		const u32 following_entry_pc = buffered.addr == -1 ?
			m_gpu_vu_generated_private_state.tpc :
			((static_cast<u32>(buffered.addr) & 0x7ffu) << 3);
		UniversalDispatchCostCacheEntry* following_cache = nullptr;
		for (UniversalDispatchCostCacheEntry& cached :
			m_gpu_vu_dispatch_cost_cache)
		{
			if (cached.valid &&
				cached.micro_generation == m_gpu_vu_micro_generation &&
				cached.entry_pc == following_entry_pc &&
				cached.configuration_bits == configuration_bits &&
				cached.vif_top == buffered.vif_top &&
				cached.vif_itop == buffered.vif_itop &&
				cached.observer_fbrst == (buffered.fbrst & 0xc00u))
			{
				following_cache = &cached;
				break;
			}
		}
		if (!following_cache)
			break;

		bool following_queued = false;
		if (following_cache->generated_state_formula_ready)
		{
			u64 formula_sequence = 0u;
			std::swap(m_deferred_vif_unpacks, buffered.unpacks);
			following_queued = TryQueueGeneratedStateLoadFormulaContinuation(
				following_cache->generated_state_formula,
				buffered.addr == -1, &formula_sequence);
			if (!following_queued)
			{
				std::swap(m_deferred_vif_unpacks, buffered.unpacks);
			}
			else
			{
				following_cache->allow_multi_execute_gather = true;
				chain_formulas++;
				chain_pairs += following_cache->pair_count;
				last_sequence = formula_sequence;
			}
		}
		else if (following_cache->generated_product_ready)
		{
			GeneratedProductHotQueueResult following_result{};
			following_queued = TryQueueGeneratedLoopKernelProductHotExecute(
				*following_cache, following_entry_pc,
				static_cast<u16>(buffered.vif_top),
				static_cast<u16>(buffered.vif_itop), &buffered.unpacks,
				&following_result, &hot_error);
			if (following_queued)
			{
				chain_draws++;
				chain_pairs += following_result.executed_pairs;
				last_sequence = following_result.sequence;
			}
		}
		if (!following_queued)
			break;

		chain_executes++;
		consumed_end = buffered.end_pos;
		vifRegs.top = buffered.vif_top;
		vifRegs.itop = buffered.vif_itop;
		vuFBRST = buffered.fbrst;
		if (VitaPerformanceTelemetry::IsEnabled() &&
			buffered.enqueued_at != 0u)
		{
			VitaGpuVu::RecordUniversalGpuVuMtvuExecuteQueueAge(
				MtvuElapsedTelemetryMicroseconds(buffered.enqueued_at));
		}
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
		Pcsx2Trace::NotifyMachineCheckpointVu1ProgramStarted();
#endif
	}

	if (consumed_end != m_read_pos)
		m_read_pos = consumed_end;
	m_gpu_vu_generated_unpublished_transactions += chain_draws;
	u32 published_draws = 0u;
	if (m_gpu_vu_generated_unpublished_transactions >=
		GeneratedLoopKernelAsyncPublicationExecutions)
	{
		const Common::Timer::Value publication_started =
			VitaPerformanceTelemetry::IsEnabled() ?
				Common::Timer::GetCurrentValue() : 0;
		published_draws = VitaGS::PublishMtvuPath1Completions();
		if (publication_started != 0)
		{
			VitaGpuVu::RecordGeneratedLoopKernelPublication(
				published_draws,
				MtvuElapsedTelemetryMicroseconds(publication_started));
		}
		if (published_draws != 0u)
		{
			m_gpu_vu_generated_unpublished_transactions = 0u;
			++m_gpu_vu_generated_async_publication_count;
		}
	}

	static u64 boundary_chain_reports = 0u;
	static u64 boundary_chain_us = 0u;
	static u64 boundary_chain_executes = 0u;
	const u64 report = ++boundary_chain_reports;
	if (chain_started != 0)
		boundary_chain_us += MtvuElapsedTelemetryMicroseconds(chain_started);
	boundary_chain_executes += chain_executes;
	if (report <= 8u || (report & (report - 1u)) == 0u)
	{
		Console.WriteLn(
			"GPU-VU seq=%llu..%llu provider=generated-chain accepted=1 "
			"product_accepted=1 executes=%u pairs=%llu descriptor_objects=%u "
			"state_formulas=%u queue_publications=%u published_draws=%u "
			"output=direct-tfx cpu_vu_calls=0 cpu_semantic_pairs=0 "
			"full_state_copies=0 transaction_commit=pending "
			"hot_boundary=mtvu-record cpu1_chain_us_per_call=%llu "
			"cpu1_chain_us_per_execute=%llu.",
			static_cast<unsigned long long>(first_sequence),
			static_cast<unsigned long long>(last_sequence), chain_executes,
			static_cast<unsigned long long>(chain_pairs), chain_draws,
			chain_formulas, published_draws != 0u ? 1u : 0u,
			published_draws,
			static_cast<unsigned long long>(boundary_chain_us / report),
			static_cast<unsigned long long>(
				boundary_chain_executes != 0u ?
					boundary_chain_us / boundary_chain_executes : 0u));
	}
	return true;
}

bool VU_Thread::TryQueueGeneratedStateLoadFormulaContinuation(
	const VitaGpuVu::UniversalStateLoadFormula& formula, bool resume,
	u64* queued_sequence_out)
{
	if (queued_sequence_out)
		*queued_sequence_out = 0u;
	if (!m_gpu_vu_generated_private_state.valid ||
		m_gpu_vu_generated_private_tail_sequence == 0u ||
		m_gpu_vu_private_state_bridge_quarantined ||
		formula.format_version !=
			VitaGpuVu::UniversalStateLoadFormulaFormatVersion ||
		formula.configuration_bits !=
			VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits() ||
		!VitaGpuVu::UniversalStateLoadFormulaEntryIsValid(
			resume, formula.start_pc,
			m_gpu_vu_generated_private_state.tpc) ||
		formula.executed_pairs == 0u ||
		formula.executed_pairs >
			VitaGpuVu::UniversalStateLoadFormulaMaximumOps ||
		m_gpu_vu_generated_private_continuations.size() >=
			MaximumGeneratedLoopKernelInFlightContinuations)
	{
		return false;
	}

	const Common::Timer::Value formula_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	GpuVuMemoryView memory_view{
		reinterpret_cast<const u8*>(state.memory.data()),
		&m_deferred_vif_unpacks,
		state.unavailable_memory_words.data(), nullptr, VU1.Mem,
		state.canonical_memory_qwords.data(),
		state.raw_memory_provenance.get()};
	VitaGpuVu::UniversalStateLoadFormulaResult evaluated;
	std::string error;
	if (!VitaGpuVu::EvaluateUniversalStateLoadFormula(
			formula, state.vi, ReadGpuVuMemoryU32, &memory_view,
			&evaluated, &error))
	{
		return false;
	}

	// Prove every pending UNPACK can be committed before mutating the private
	// generation. The formula's memory reads already observed these spans in
	// command order through GpuVuMemoryView; this pass establishes that the same
	// complete bytes can update the deferred canonical image without failure.
	auto next_unavailable = state.unavailable_memory_words;
	for (const VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
	{
		u32 payload_size = 0u;
		const u8* const payload = VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!payload ||
			!VitaGpuVu::GetDirectAffineV4_32PayloadSize(span, &payload_size) ||
			payload_size != span.payload.size)
		{
			return false;
		}
		GeneratedLoopKernelPrivateState::MarkCompleteQwordSpanAvailable(
			&next_unavailable, span.destination_qword, span.vector_count);
	}

	GeneratedPrivateStateLoadContinuation continuation;
	continuation.predecessor_sequence =
		m_gpu_vu_generated_private_tail_sequence;
	continuation.sequence = VitaGpuVu::NextGpuVuOrderingSequence();
	continuation.final_tpc = formula.final_tpc;
	continuation.final_cycle = formula.cycle_count;
	continuation.executed_pairs = formula.executed_pairs;
	continuation.compiled_state_formula = true;
	continuation.unavailable_memory_words = next_unavailable;
	for (u32 reg = 1u; reg < evaluated.vf_lane_masks.size(); reg++)
	{
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u8 bit = static_cast<u8>(0x8u >> lane);
			if ((evaluated.vf_lane_masks[reg] & bit) == 0u)
				continue;
			const u32 value = evaluated.vf_values[reg][lane];
			if (value != state.vf[reg][lane])
			{
				continuation.vf_lane_masks[reg] |= bit;
				continuation.vf_values[reg][lane] = value;
			}
		}
	}
	for (u32 reg = 1u; reg < evaluated.vi.size(); reg++)
	{
		if ((formula.vi_write_mask & (1u << reg)) == 0u ||
			evaluated.vi[reg] == state.vi[reg])
		{
			continue;
		}
		continuation.vi_write_mask |= 1u << reg;
		continuation.vi_values[reg] = evaluated.vi[reg];
	}

	// All rejection points are above. Commit the compact assignment formula to
	// the CPU1-private generation; this performs no PairPlan dispatch, pipeline
	// step, full VU-state copy, or whole-state fingerprint.
	for (const VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
	{
		pxAssertRel(RetainOrMaterializeGpuVuPrivateUnpack(
			state.raw_memory_provenance.get(), span, state.memory.data()),
			"preflighted state-formula UNPACK lost its immutable input");
		state.ApplyCompleteQwordUnpackOwnership(
			span.destination_qword, span.vector_count);
	}
	state.ReleaseDeadDeferredMemoryOwners();
	state.unavailable_memory_words = next_unavailable;
	for (u32 reg = 1u; reg < evaluated.vf_lane_masks.size(); reg++)
	{
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u8 bit = static_cast<u8>(0x8u >> lane);
			if ((evaluated.vf_lane_masks[reg] & bit) == 0u)
				continue;
			state.vf[reg][lane] = evaluated.vf_values[reg][lane];
			state.unavailable_vf_lanes[reg] &= ~bit;
			state.ClearDeferredRegisterOwnerSlot(
				&state.deferred_vf_owner_slots[reg * 4u + lane]);
		}
	}
	for (u32 reg = 1u; reg < evaluated.vi.size(); reg++)
	{
		if ((formula.vi_write_mask & (1u << reg)) != 0u)
			state.vi[reg] = evaluated.vi[reg];
	}
	state.vi[0] = 0u;
	state.tpc = formula.final_tpc;
	continuation.replay_unpacks = std::move(m_deferred_vif_unpacks);
	m_deferred_vif_unpacks.clear();
	m_deferred_vif_unpack_count.store(0u, std::memory_order_release);

	const u64 queued_sequence = continuation.sequence;
	const u64 queued_predecessor = continuation.predecessor_sequence;
	const bool predecessor_pending = std::any_of(
		m_gpu_vu_generated_pending_transactions.begin(),
		m_gpu_vu_generated_pending_transactions.end(),
		[queued_predecessor](const auto& transaction) {
			return transaction &&
				transaction->Sequence() == queued_predecessor;
		});
	if (predecessor_pending)
	{
		m_gpu_vu_generated_private_continuations.push_back(
			std::move(continuation));
	}
	else
	{
		for (VitaGpuVu::VifUnpackSpan& span : continuation.replay_unpacks)
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
		continuation.replay_unpacks.clear();
		RecordGeneratedLoopKernelPrivateCompletion(formula.cycle_count);
		++m_gpu_vu_private_state_load_adopted_count;
	}
	PublishGeneratedLoopKernelPendingExecutionCount();
	m_execute_jobs_completed.fetch_add(1u, std::memory_order_release);
#if defined(__vita__)
	VitaGpuVu::HealthJournal::CountNoOutputExecution(
		VitaGpuVu::HealthJournal::NoOutputExecutionKind::StateFormula);
#endif
	VitaGS::CompleteMtvuPath1NoOutput();
	m_ring_space_progress.NotifyOfProgress();
	VitaGpuVu::RecordUniversalGpuVuAsyncQueued(
		GeneratedLoopKernelPendingExecutionCount());
	VitaGpuVu::RecordGeneratedLoopKernelStateFormula(
		formula.executed_pairs, formula.operation_count,
		formula_started != 0 ?
			MtvuElapsedTelemetryMicroseconds(formula_started) : 0u);
	if (queued_sequence_out)
		*queued_sequence_out = queued_sequence;

	const u64 queued_count = ++m_gpu_vu_private_state_load_queued_count;
	if (queued_count <= 8u || (queued_count & (queued_count - 1u)) == 0u)
	{
		Console.WriteLn(
			"GPU-VU seq=%llu provider=host-state-formula accepted=1 "
			"logical_pairs=%u formula_ops=%u output=none bridge_count=%llu "
			"predecessor=%llu pending_batch=%u cpu_vu_calls=0 "
			"cpu_semantic_pairs=0 full_state_copies=0 fingerprints=0 "
			"transaction_commit=pending.",
			static_cast<unsigned long long>(queued_sequence),
			formula.executed_pairs, formula.operation_count,
			static_cast<unsigned long long>(queued_count),
			static_cast<unsigned long long>(queued_predecessor),
			GeneratedLoopKernelPendingExecutionCount());
	}
	return true;
}

bool VU_Thread::TryQueueGeneratedPrivateStateLoadContinuation(
	const VitaGpuVu::UniversalGpuVuEpoch& epoch, u32 fbrst,
	bool resume, bool allow_general_no_output,
	VitaGpuVu::UniversalStateLoadFormula* compiled_formula)
{
	static constexpr u32 MaximumPrivateNoOutputContinuationPairs = 512u;
	const u32 maximum_pairs = allow_general_no_output ?
		MaximumPrivateNoOutputContinuationPairs : 32u;
	if (!m_gpu_vu_generated_private_state.valid ||
		m_gpu_vu_generated_private_tail_sequence == 0u ||
		m_gpu_vu_private_state_bridge_quarantined ||
		epoch.ExecuteCount() != 1u ||
		epoch.PreflightPairCount() == 0u ||
		epoch.PreflightPairCount() > maximum_pairs ||
		(!allow_general_no_output &&
		 epoch.DynamicPairUpperBound() > maximum_pairs) ||
		m_gpu_vu_generated_private_continuations.size() >=
			MaximumGeneratedLoopKernelInFlightContinuations)
	{
		return false;
	}

	// Compile every small observer-free connector to an immutable assignment
	// formula before considering the CPU PairPlan bridge. Previously the general
	// no-PATH1 path skipped this test by construction, so BSpline paid a full
	// 16 KiB private-state copy, seven CPU pairs and two whole-generation
	// fingerprints once every three generated draws. Formula construction is
	// pre-effect and title-neutral; an unsupported operation simply continues to
	// the exact private reference below.
	{
		VitaGpuVu::UniversalStateLoadFormula formula;
		std::string formula_error;
		const bool formula_built = VitaGpuVu::BuildUniversalStateLoadFormula(
				epoch.Program(), std::min<u32>(maximum_pairs,
					VitaGpuVu::UniversalStateLoadFormulaMaximumOps),
				&formula, &formula_error);
		if (formula_built &&
			TryQueueGeneratedStateLoadFormulaContinuation(formula, resume))
		{
			if (compiled_formula)
				*compiled_formula = formula;
			return true;
		}
		// This is a support-ledger event, not a workload matcher.  The exact
		// PairPlan compiler should explain why an observer-free connector still
		// enters the expensive copied-state CPU bridge; previously the rejection
		// was silently discarded and made CPU1's remaining ownership invisible.
		static u64 formula_rejection_count = 0u;
		const u64 rejection = ++formula_rejection_count;
		if (rejection <= 8u || (rejection & (rejection - 1u)) == 0u)
		{
			Console.WriteLn(
				"GPU-VU: generated state formula rejected identity=%016llx "
				"entry=%04x pairs=%u built=%u reason=%s pre_effect=1 count=%llu.",
				static_cast<unsigned long long>(epoch.ProgramIdentity()),
				epoch.AnalysisEntryPc(), epoch.PreflightPairCount(),
				formula_built ? 1u : 0u,
				formula_error.empty() ?
					"private-state-input-unavailable" : formula_error.c_str(),
				static_cast<unsigned long long>(rejection));
		}
	}
	if (!allow_general_no_output)
	{
		return false;
	}
	if (!MaterializeGeneratedLoopKernelDeferredState())
		return false;
	const Common::Timer::Value bridge_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;

	GeneratedLoopKernelPrivateState candidate =
		m_gpu_vu_generated_private_state;
	const auto set_word_available = [&candidate](u32 word) {
		candidate.unavailable_memory_words[word >> 5] &=
			~(1u << (word & 31u));
		candidate.ClearUnavailableMemoryOwner(word);
		const u32 qword = word >> 2u;
		candidate.canonical_memory_qwords[qword >> 5] &=
			~(1u << (qword & 31u));
	};
	for (const VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
	{
		u32 payload_size = 0u;
		const u8* const payload =
			VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!payload ||
			!VitaGpuVu::GetDirectAffineV4_32PayloadSize(
				span, &payload_size) ||
			payload_size != span.payload.size ||
			!VitaGpuVu::MaterializeDirectAffineV4_32Span(
				span, payload, candidate.memory.data(), VU1_MEMSIZE))
		{
			return false;
		}
		for (u32 vector = 0u; vector < span.vector_count; vector++)
		{
			const u32 qword =
				(static_cast<u32>(span.destination_qword) + vector) & 0x3ffu;
			for (u32 lane = 0u; lane < 4u; lane++)
				set_word_available(qword * 4u + lane);
		}
	}

	VURegs scratch;
	std::memset(&scratch, 0, sizeof(scratch));
	scratch.Mem = reinterpret_cast<u8*>(candidate.memory.data());
	scratch.Micro = VU1.Micro;
	scratch.idx = 1u;
	std::memcpy(&scratch.VF[0].UL[0], &candidate.vf[0][0],
		sizeof(candidate.vf));
	std::memcpy(&scratch.ACC.UL[0], candidate.acc.data(),
		sizeof(candidate.acc));
	for (u32 reg = 0u; reg < candidate.vi.size(); reg++)
		scratch.VI[reg].US[0] = candidate.vi[reg];
	scratch.VI[REG_Q].UL = candidate.q;
	scratch.VI[REG_P].UL = candidate.p;
	scratch.VI[REG_I].UL = candidate.i;
	scratch.VI[REG_TPC].UL = epoch.AnalysisEntryPc() >> 3;

	VitaGpuVu::UniversalReferenceRunResult execution;
	std::string error;
	const auto unavailable_before = candidate.unavailable_memory_words;
	const bool executed = allow_general_no_output ?
		VitaGpuVu::ExecuteUniversalPrivateNoOutputReference(
			&scratch, epoch.Program(), maximum_pairs, fbrst,
			static_cast<u16>(vifRegs.top),
			static_cast<u16>(vifRegs.itop),
			candidate.unavailable_memory_words.data(),
			static_cast<u32>(candidate.unavailable_memory_words.size()),
			&execution, &error) :
		VitaGpuVu::ExecuteUniversalPrivateStateLoadReference(
			&scratch, epoch.Program(), maximum_pairs, fbrst,
			candidate.unavailable_memory_words.data(),
			static_cast<u32>(candidate.unavailable_memory_words.size()),
			&execution, &error);
	if (!executed)
	{
		if (allow_general_no_output)
		{
			static u64 rejection_count = 0u;
			const u64 count = ++rejection_count;
			if (count <= 8u || (count & (count - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU: private no-output continuation rejected "
					"identity=%016llx entry=%04x pairs=%u stop_pc=%04x "
					"reason=%s pre_effect=1 count=%llu.",
					static_cast<unsigned long long>(epoch.ProgramIdentity()),
					epoch.AnalysisEntryPc(), execution.executed_pairs,
					execution.stop_pc,
					error.empty() ? "private reference rejected" : error.c_str(),
					static_cast<unsigned long long>(count));
			}
		}
		return false;
	}
	if (!allow_general_no_output &&
		(std::memcmp(&scratch.ACC.UL[0], candidate.acc.data(),
			sizeof(candidate.acc)) != 0 ||
		 scratch.VI[REG_Q].UL != candidate.q ||
		 scratch.VI[REG_P].UL != candidate.p ||
		 scratch.VI[REG_I].UL != candidate.i))
	{
		return false;
	}
	if (allow_general_no_output)
	{
		for (u32 mask_word = 0u;
			mask_word < execution.written_memory_qwords.size(); mask_word++)
		{
			const u32 written_qwords =
				execution.written_memory_qwords[mask_word];
			candidate.canonical_memory_qwords[mask_word] &=
				~written_qwords;
		}
		for (u32 word = 0u; word < candidate.memory.size(); word++)
		{
			const u32 bit = 1u << (word & 31u);
			if ((unavailable_before[word >> 5] & bit) != 0u &&
				(candidate.unavailable_memory_words[word >> 5] & bit) == 0u)
			{
				candidate.ClearUnavailableMemoryOwner(word);
			}
		}
		std::memcpy(candidate.acc.data(), &scratch.ACC.UL[0],
			sizeof(candidate.acc));
		candidate.q = scratch.VI[REG_Q].UL;
		candidate.p = scratch.VI[REG_P].UL;
		candidate.i = scratch.VI[REG_I].UL;
		candidate.ReleaseDeadDeferredMemoryOwners();
	}

	GeneratedPrivateStateLoadContinuation continuation;
	continuation.predecessor_sequence =
		m_gpu_vu_generated_private_tail_sequence;
	continuation.sequence = VitaGpuVu::NextGpuVuOrderingSequence();
	continuation.final_tpc = execution.stop_pc;
	continuation.final_cycle = static_cast<u32>(scratch.cycle);
	continuation.executed_pairs = execution.executed_pairs;
	for (u32 reg = 1u; reg < candidate.vf.size(); reg++)
	{
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			const u32 value = scratch.VF[reg].UL[lane];
			if (value == candidate.vf[reg][lane])
				continue;
			continuation.vf_lane_masks[reg] |= 0x8u >> lane;
			continuation.vf_values[reg][lane] = value;
			candidate.vf[reg][lane] = value;
		}
	}
	for (u32 reg = 1u; reg < candidate.vi.size(); reg++)
	{
		const u16 value = scratch.VI[reg].US[0];
		if (value == candidate.vi[reg])
			continue;
		continuation.vi_write_mask |= 1u << reg;
		continuation.vi_values[reg] = value;
		candidate.vi[reg] = value;
	}
	candidate.vi[0] = 0u;
	candidate.tpc = execution.stop_pc;
	continuation.unavailable_memory_words =
		candidate.unavailable_memory_words;
	continuation.expected_memory_fingerprint = FingerprintGpuVuMemory(
		candidate.memory.data(), candidate.unavailable_memory_words.data(),
		static_cast<u32>(candidate.memory.size()));
	continuation.expected_register_fingerprint = FingerprintGpuVuRegisters(
		&candidate.vf[0][0], candidate.acc.data(), candidate.vi.data(),
		candidate.q, candidate.p, candidate.i, candidate.tpc);
	continuation.replay_unpacks = std::move(m_deferred_vif_unpacks);
	m_deferred_vif_unpacks.clear();
	m_deferred_vif_unpack_count.store(0u, std::memory_order_release);
	m_gpu_vu_generated_private_state = std::move(candidate);
	GeneratedLoopKernelPrivateState& committed_state =
		m_gpu_vu_generated_private_state;
	if (!committed_state.raw_memory_provenance)
	{
		committed_state.raw_memory_provenance =
			VitaGpuVu::PersistentVifMemoryProvenance::Create();
	}
	if (committed_state.raw_memory_provenance)
	{
		bool provenance_valid = true;
		for (const VitaGpuVu::VifUnpackSpan& span : continuation.replay_unpacks)
		{
			provenance_valid &= committed_state.raw_memory_provenance->
				ApplyDirectAffineSpan(span);
		}
		if (!provenance_valid)
		{
			committed_state.raw_memory_provenance->Clear();
		}
		else
		{
			for (u32 mask_word = 0u;
				mask_word < execution.written_memory_qwords.size(); mask_word++)
			{
				const u32 written = execution.written_memory_qwords[mask_word];
				for (u32 bit = 0u; bit < 32u; bit++)
				{
					if ((written & (1u << bit)) != 0u)
					{
						committed_state.raw_memory_provenance->InvalidateQword(
							static_cast<u16>(mask_word * 32u + bit));
					}
				}
			}
		}
	}
	const u64 queued_sequence = continuation.sequence;
	const u64 queued_predecessor = continuation.predecessor_sequence;
	const u32 queued_pairs = continuation.executed_pairs;
	const bool predecessor_pending = std::any_of(
		m_gpu_vu_generated_pending_transactions.begin(),
		m_gpu_vu_generated_pending_transactions.end(),
		[queued_predecessor](const auto& transaction) {
			return transaction && transaction->Sequence() == queued_predecessor;
		});
	if (predecessor_pending)
	{
		m_gpu_vu_generated_private_continuations.push_back(
			std::move(continuation));
	}
	else
	{
		for (VitaGpuVu::VifUnpackSpan& span : continuation.replay_unpacks)
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
		continuation.replay_unpacks.clear();
		RecordGeneratedLoopKernelPrivateCompletion(continuation.final_cycle);
		++m_gpu_vu_private_state_load_adopted_count;
	}
	PublishGeneratedLoopKernelPendingExecutionCount();
	// The EE-side latency counter tracks CPU1 ownership handoff, not physical
	// GXM retirement. This private successor is already exact and available to
	// following descriptors; observers still drain the transaction journal.
	m_execute_jobs_completed.fetch_add(1u, std::memory_order_release);
#if defined(__vita__)
	VitaGpuVu::HealthJournal::CountNoOutputExecution(allow_general_no_output ?
		VitaGpuVu::HealthJournal::NoOutputExecutionKind::PrivateGeneral :
		VitaGpuVu::HealthJournal::NoOutputExecutionKind::PrivateStateLoad);
#endif
	VitaGS::CompleteMtvuPath1NoOutput();
	m_ring_space_progress.NotifyOfProgress();
	VitaGpuVu::RecordUniversalGpuVuAsyncQueued(
		GeneratedLoopKernelPendingExecutionCount());

	if (bridge_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelPrivateBridge(
			queued_pairs,
			MtvuElapsedTelemetryMicroseconds(bridge_started));
	}
	const u64 queued_count = ++m_gpu_vu_private_state_load_queued_count;
	if (queued_count <= 8u || (queued_count & (queued_count - 1u)) == 0u)
	{
		Console.WriteLn(
			"GPU-VU seq=%llu provider=%s accepted=1 "
			"state_bridge=transactional pairs=%u output=none "
			"bridge_count=%llu predecessor=%llu pending_batch=%u cpu_vu_calls=0 "
			"cpu_private_pairs=%u transaction_commit=pending.",
			static_cast<unsigned long long>(queued_sequence),
			allow_general_no_output ? "cpu-private-no-output" :
				"cpu-private-state-load",
			queued_pairs,
			static_cast<unsigned long long>(queued_count),
			static_cast<unsigned long long>(queued_predecessor),
			GeneratedLoopKernelPendingExecutionCount(),
			queued_pairs);
	}
	return true;
}

bool VU_Thread::ApplyGeneratedPrivateStateLoadContinuations(
	u64 predecessor_sequence)
{
	while (!m_gpu_vu_generated_private_continuations.empty() &&
		m_gpu_vu_generated_private_continuations.front().predecessor_sequence ==
			predecessor_sequence)
	{
		GeneratedPrivateStateLoadContinuation& continuation =
			m_gpu_vu_generated_private_continuations.front();
		ReplayVifUnpackSpans(&continuation.replay_unpacks);
		for (u32 reg = 1u; reg < continuation.vf_lane_masks.size(); reg++)
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((continuation.vf_lane_masks[reg] & (0x8u >> lane)) != 0u)
					VU1.VF[reg].UL[lane] = continuation.vf_values[reg][lane];
			}
		}
		for (u32 reg = 1u; reg < continuation.vi_values.size(); reg++)
		{
			if ((continuation.vi_write_mask & (1u << reg)) != 0u)
				VU1.VI[reg].US[0] = continuation.vi_values[reg];
		}
		VU1.VI[0].UL = 0u;
		VU1.VI[REG_TPC].UL = continuation.final_tpc >> 3;
		std::array<u16, 16> canonical_vi{};
		for (u32 reg = 0u; reg < canonical_vi.size(); reg++)
			canonical_vi[reg] = VU1.VI[reg].US[0];
		const u64 memory_fingerprint = continuation.compiled_state_formula ? 0u :
			FingerprintGpuVuMemory(
				reinterpret_cast<const u32*>(VU1.Mem),
				continuation.unavailable_memory_words.data(),
				GeneratedPrivateStateLoadContinuation::MemoryWordCount);
		const u64 register_fingerprint = continuation.compiled_state_formula ? 0u :
			FingerprintGpuVuRegisters(
				&VU1.VF[0].UL[0], &VU1.ACC.UL[0], canonical_vi.data(),
				VU1.VI[REG_Q].UL, VU1.VI[REG_P].UL, VU1.VI[REG_I].UL,
				VU1.VI[REG_TPC].UL << 3);
		const bool state_matches = continuation.compiled_state_formula ||
			(memory_fingerprint == continuation.expected_memory_fingerprint &&
			 register_fingerprint == continuation.expected_register_fingerprint);
		if (!state_matches)
			m_gpu_vu_private_state_bridge_quarantined = true;

		BeginProgram();
		EndProgram(InterruptFlagVUEBit);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
		Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
		vuCycles[vuCycleIdx].store(
			continuation.final_cycle, std::memory_order_release);
		vuCycleIdx = (vuCycleIdx + 1) & 3;
		const u64 adopted_count =
			++m_gpu_vu_private_state_load_adopted_count;
		if (adopted_count <= 8u ||
			(adopted_count & (adopted_count - 1u)) == 0u)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=%s "
				"transaction_commit=adopted bridge_count=%llu pairs=%u output=none "
				"state_attestation=%s memory=%016llx/%016llx "
				"registers=%016llx/%016llx cpu_vu_calls=0 "
				"cpu_semantic_pairs=%u formula_ops_cached=%u.",
				static_cast<unsigned long long>(continuation.sequence),
				continuation.compiled_state_formula ? "host-state-formula" :
					"cpu-private-state-load",
				static_cast<unsigned long long>(adopted_count),
				continuation.executed_pairs,
				state_matches ? "passed" : "FAILED",
				static_cast<unsigned long long>(memory_fingerprint),
				static_cast<unsigned long long>(
					continuation.expected_memory_fingerprint),
				static_cast<unsigned long long>(register_fingerprint),
				static_cast<unsigned long long>(
					continuation.expected_register_fingerprint),
				continuation.compiled_state_formula ? 0u :
					continuation.executed_pairs,
				continuation.compiled_state_formula ? 1u : 0u);
		}
		m_gpu_vu_generated_private_continuations.pop_front();
		PublishGeneratedLoopKernelPendingExecutionCount();
	}
	return true;
}

void VU_Thread::InvalidateGeneratedLoopKernelPrivateState()
{
	GeneratedLoopKernelPrivateState& state =
		m_gpu_vu_generated_private_state;
	state.valid = false;
	if (state.raw_memory_provenance)
		state.raw_memory_provenance->Clear();
	state.canonical_memory_qwords.fill(0u);
	state.unavailable_memory_words.fill(0u);
	state.unavailable_memory_owner_sequence.fill(0u);
	state.unavailable_memory_owner_slot.fill(0u);
	state.native_memory_owners = {};
	state.deferred_memory_owners.clear();
	state.unavailable_vf_lanes.fill(0u);
	state.ResetDeferredRegisterOwners();
	state.unavailable_acc_lanes = 0u;
	state.q_available = true;
	state.p_available = true;
	state.i_available = true;
}

bool VU_Thread::AdoptGeneratedLoopKernelTransactionBatch()
{
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
	// These owners intentionally observe every intermediate architectural
	// generation. Product execution instead publishes only at the real drain
	// boundary below.
	return false;
#else
	if (!RetireCompletedGeneratedLoopKernelTransactions() ||
		!m_gpu_vu_generated_pending_transactions.empty())
	{
		return false;
	}
	return CommitGeneratedLoopKernelPrivateState();

#if 0
	if (m_gpu_vu_generated_pending_transactions.empty() ||
		!m_gpu_vu_generated_private_state.valid)
	{
		return false;
	}

	// Validate the complete batch before touching either the final private
	// generation or canonical VU1 state. Native output-only words are filled
	// from the newest GPU writer in issue order; a later exact UNPACK/store has
	// already made that word available in the private generation and therefore
	// suppresses the older GPU value.
	auto unresolved =
		m_gpu_vu_generated_private_state.unavailable_memory_words;
	for (const auto& transaction : m_gpu_vu_generated_pending_transactions)
	{
		if (!transaction ||
			transaction->Stage() !=
				VitaGpuVu::GeneratedLoopKernelTransactionStage::GpuCompleted ||
			transaction->StoreTargets().empty() ||
			transaction->OutputWords().size() !=
				transaction->OutputWordCount() ||
			transaction->CommittedStoreWords().size() !=
				transaction->OutputWordCount())
		{
			return false;
		}
	}
	for (auto transaction_it =
			m_gpu_vu_generated_pending_transactions.rbegin();
		 transaction_it != m_gpu_vu_generated_pending_transactions.rend();
		 ++transaction_it)
	{
		const auto& transaction = **transaction_it;
		for (const VitaGpuVu::GeneratedLoopKernelStoreTarget& target :
			transaction.StoreTargets())
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) == 0u)
					continue;
				const u32 word =
					static_cast<u32>(target.address_qword) * 4u + lane;
				unresolved[word >> 5] &= ~(1u << (word & 31u));
			}
		}
	}
	if (std::any_of(unresolved.begin(), unresolved.end(),
			[](u32 word) { return word != 0u; }))
	{
		return false;
	}

	const Common::Timer::Value commit_started =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
	GeneratedLoopKernelPrivateState& final_state =
		m_gpu_vu_generated_private_state;
	for (auto transaction_it =
			m_gpu_vu_generated_pending_transactions.rbegin();
		 transaction_it != m_gpu_vu_generated_pending_transactions.rend();
		 ++transaction_it)
	{
		const auto& transaction = **transaction_it;
		const auto& targets = transaction.StoreTargets();
		const auto& words = transaction.CommittedStoreWords();
		for (u32 entry = 0u; entry < targets.size(); entry++)
		{
			const auto& target = targets[entry];
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) == 0u)
					continue;
				const u32 word =
					static_cast<u32>(target.address_qword) * 4u + lane;
				const u32 bit = 1u << (word & 31u);
				if ((final_state.unavailable_memory_words[word >> 5] & bit) == 0u)
					continue;
				final_state.memory[word] = words[entry * 4u + lane];
				final_state.unavailable_memory_words[word >> 5] &= ~bit;
			}
		}
	}
	const Common::Timer::Value resolve_finished = commit_started != 0 ?
		Common::Timer::GetCurrentValue() : 0;

	// CPU1 has already advanced this exact private successor while each GPU
	// transaction was queued. Materialize it once at the observer/pressure
	// boundary instead of replaying every intermediate UNPACK, store journal,
	// register file and continuation into canonical VU1 state.
	std::memcpy(VU1.Mem, final_state.memory.data(), VU1_MEMSIZE);
	std::memcpy(&VU1.VF[0].UL[0], &final_state.vf[0][0],
		sizeof(final_state.vf));
	std::memcpy(&VU1.ACC.UL[0], final_state.acc.data(),
		sizeof(final_state.acc));
	for (u32 reg = 0u; reg < final_state.vi.size(); reg++)
		VU1.VI[reg].US[0] = final_state.vi[reg];
	VU1.VI[0].UL = 0u;
	VU1.VI[REG_Q].UL = final_state.q;
	VU1.VI[REG_P].UL = final_state.p;
	VU1.VI[REG_I].UL = final_state.i;
	VU1.VI[REG_TPC].UL = final_state.tpc >> 3;
	const Common::Timer::Value canonical_copy_finished = commit_started != 0 ?
		Common::Timer::GetCurrentValue() : 0;

	const u32 transaction_count = static_cast<u32>(
		m_gpu_vu_generated_pending_transactions.size());
	u32 continuation_count = 0u;
	u64 accepted_pairs = 0u;
	while (!m_gpu_vu_generated_pending_transactions.empty())
	{
		const std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>&
			transaction = m_gpu_vu_generated_pending_transactions.front();
		const u64 sequence = transaction->Sequence();
		const u32 pairs = transaction->ExecutedPairs();
		if (!transaction->MarkAdopted())
			return false;
		BeginProgram();
		EndProgram(InterruptFlagVUEBit);
		vuCycles[vuCycleIdx].store(4u, std::memory_order_release);
		vuCycleIdx = (vuCycleIdx + 1) & 3;
		VitaGpuVu::RecordGeneratedLoopKernelAccepted(pairs);
		VitaGpuVu::RecordUniversalGpuVuProviderCompletion(true, 0u, 0u);
		accepted_pairs += pairs;
		m_gpu_vu_generated_pending_transactions.pop_front();

		while (!m_gpu_vu_generated_private_continuations.empty() &&
			m_gpu_vu_generated_private_continuations.front().predecessor_sequence ==
				sequence)
		{
			GeneratedPrivateStateLoadContinuation& continuation =
				m_gpu_vu_generated_private_continuations.front();
			for (VitaGpuVu::VifUnpackSpan& span : continuation.replay_unpacks)
				VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continuation.replay_unpacks.clear();
			BeginProgram();
			EndProgram(InterruptFlagVUEBit);
			vuCycles[vuCycleIdx].store(
				continuation.final_cycle, std::memory_order_release);
			vuCycleIdx = (vuCycleIdx + 1) & 3;
			++m_gpu_vu_private_state_load_adopted_count;
			continuation_count++;
			m_gpu_vu_generated_private_continuations.pop_front();
		}
	}
	if (!m_gpu_vu_generated_private_continuations.empty())
	{
		Console.Error(
			"GPU-VU: coalesced final generation left an orphan continuation.");
		return false;
	}

	PublishGeneratedLoopKernelPendingExecutionCount();
	const Common::Timer::Value retirement_finished = commit_started != 0 ?
		Common::Timer::GetCurrentValue() : 0;
	const u64 commit_us = commit_started != 0 ?
		MtvuElapsedTelemetryMicroseconds(commit_started) : 0u;
	if (commit_started != 0)
		VitaGpuVu::RecordGeneratedLoopKernelBatchCommit(commit_us);
	const auto elapsed_between = [](Common::Timer::Value begin,
		Common::Timer::Value end) -> u64 {
		return begin != 0 && end >= begin ? static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(end - begin) * 1000000.0) : 0u;
	};
	Console.WriteLn(
		"GPU-VU generated_batch_commit=coalesced transactions=%u "
		"continuations=%u pairs=%llu canonical_copies=1 memory_bytes=%u "
		"intermediate_replays=0 resolve_us=%llu canonical_copy_us=%llu "
		"retire_us=%llu commit_us=%llu transaction_commit=adopted.",
		transaction_count, continuation_count,
		static_cast<unsigned long long>(accepted_pairs), VU1_MEMSIZE,
		static_cast<unsigned long long>(elapsed_between(
			commit_started, resolve_finished)),
		static_cast<unsigned long long>(elapsed_between(
			resolve_finished, canonical_copy_finished)),
		static_cast<unsigned long long>(elapsed_between(
			canonical_copy_finished, retirement_finished)),
		static_cast<unsigned long long>(commit_us));
	InvalidateGeneratedLoopKernelPrivateState();
	return true;
#endif
#endif
}

bool VU_Thread::DrainGeneratedLoopKernelTransactions(
	bool wait_for_all, bool commit_private_state)
{
#if !defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_PORTABLE_REPLAY_VALIDATION) && \
	!defined(VITASX2_PRODUCT_BOOT_VALIDATION) && \
	!defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
	if (!RetireCompletedGeneratedLoopKernelTransactions())
		return false;
	if (!wait_for_all)
		return true;

	const u32 initial_pending = GeneratedLoopKernelPendingExecutionCount();
	const u64 drain_count = initial_pending != 0u ?
		++m_gpu_vu_generated_batch_drain_count : 0u;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (initial_pending != 0u)
	{
		PublishGpuVuHealth(
			static_cast<u32>(GpuVuHealthCpu1Stage::DrainBegin));
	}
#endif
	const bool report_drain = drain_count != 0u &&
		(drain_count <= 8u || (drain_count & (drain_count - 1u)) == 0u);
	if (report_drain)
	{
		Console.WriteLn(
			"GPU-VU generated_generation_drain=begin reason=%s drain_count=%llu "
			"physical_pending=%u retired_transactions=%llu logical_pending=%u.",
			commit_private_state ? "architectural-observer" : "capacity",
			static_cast<unsigned long long>(drain_count),
			static_cast<u32>(m_gpu_vu_generated_pending_transactions.size()),
			static_cast<unsigned long long>(
				m_gpu_vu_generated_private_retired_transactions),
			initial_pending);
	}

	Common::Timer::Value drain_wait_started = 0;
	u64 wait_polls = 0u;
	if (!m_gpu_vu_generated_pending_transactions.empty())
	{
		const u64 first_handoff_sequence =
			m_gpu_vu_generated_pending_transactions.front()->Sequence();
		const u64 last_handoff_sequence =
			m_gpu_vu_generated_pending_transactions.back()->Sequence();
		if (!VitaGS::ArmGeneratedGpuVuHandoffWatchdog(
				VitaGS::GeneratedGpuVuHandoffStage::CompletionPublish,
				first_handoff_sequence))
		{
			Console.Error(
				"GPU-VU seq=%llu..%llu could not arm the generated handoff "
				"watchdog before an irreversible drain.",
				static_cast<unsigned long long>(first_handoff_sequence),
				static_cast<unsigned long long>(last_handoff_sequence));
			return false;
		}
		struct GeneratedHandoffWatchdogScope final
		{
			~GeneratedHandoffWatchdogScope()
			{
				// MTGS may extend the watched tail when a later descriptor is
				// joined to the same ordered publication. A finite CPU1 return
				// must clear that whole process-wide scope, not only the tail
				// which was visible before FlushMtvuPath1Completions().
				VitaGS::CompleteGeneratedGpuVuHandoffWatchdog(
					std::numeric_limits<u64>::max());
			}
		} handoff_watchdog_scope;
		drain_wait_started = VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;
		VitaGS::FlushMtvuPath1Completions();
		m_gpu_vu_generated_unpublished_transactions = 0;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuHealth(
			static_cast<u32>(GpuVuHealthCpu1Stage::DrainSubmitted));
#endif
		for (;;)
		{
			if (!RetireCompletedGeneratedLoopKernelTransactions())
			{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
				PublishGpuVuHealth(
					static_cast<u32>(GpuVuHealthCpu1Stage::DrainFailed),
					static_cast<u32>(std::min<u64>(wait_polls,
						std::numeric_limits<u32>::max())));
#endif
				return false;
			}
			if (m_gpu_vu_generated_pending_transactions.empty())
				break;
			VitaGS::NotifyUniversalGpuVuProgress();
			Threading::Sleep(1);
			wait_polls++;
			if (wait_polls == 1u ||
				(wait_polls & (wait_polls - 1u)) == 0u)
			{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
				PublishGpuVuHealth(
					static_cast<u32>(GpuVuHealthCpu1Stage::DrainWaiting),
					static_cast<u32>(std::min<u64>(wait_polls,
						std::numeric_limits<u32>::max())));
#endif
			}
			if (wait_polls == 100u ||
				(wait_polls > 100u &&
				 (wait_polls & (wait_polls - 1u)) == 0u))
			{
				Console.WriteLn(
					"GPU-VU generated_generation_wait=poll drain_count=%llu "
					"polls=%llu physical_pending=%u first_sequence=%llu.",
					static_cast<unsigned long long>(drain_count),
					static_cast<unsigned long long>(wait_polls),
					static_cast<u32>(
						m_gpu_vu_generated_pending_transactions.size()),
					static_cast<unsigned long long>(
						m_gpu_vu_generated_pending_transactions.front()->Sequence()));
			}
		}
	}
	if (drain_wait_started != 0)
	{
		VitaGpuVu::RecordGeneratedLoopKernelBatchDrainWait(
			MtvuElapsedTelemetryMicroseconds(drain_wait_started), wait_polls);
	}
	if (commit_private_state)
	{
		if (!CommitGeneratedLoopKernelPrivateState())
			return false;
	}
	else if (report_drain)
	{
		// The transaction/resource bound is not a guest-visible VU observer.
		// Retain the exact private generation and its lazily evaluated store
		// owners after physical GXM retirement.  The old path materialized every
		// output-store expression graph here, effectively replaying generated VU
		// arithmetic on CPU1 every 24 logical operations.
			Console.WriteLn(
				"GPU-VU generated_capacity_retirement=private-retained "
				"drain_count=%llu logical_completions=%llu "
				"deferred_store_owners=%u canonical_copies=0 "
				"cpu_store_evaluations=0 "
				"deferred_successor_evaluations_total=%llu.",
				static_cast<unsigned long long>(drain_count),
				static_cast<unsigned long long>(
					m_gpu_vu_generated_private_completion_count),
				static_cast<u32>(
					m_gpu_vu_generated_private_state.deferred_memory_owners.size()),
				static_cast<unsigned long long>(
					m_gpu_vu_generated_deferred_successor_evaluations));
	}
	if (report_drain)
	{
		Console.WriteLn(
			"GPU-VU generated_generation_drain=complete reason=%s "
			"drain_count=%llu physical_pending=0 private_tpc=%04x "
			"canonical_committed=%u waits=%llu.",
			commit_private_state ? "architectural-observer" : "capacity",
			static_cast<unsigned long long>(drain_count),
			m_gpu_vu_generated_private_state.valid ?
				m_gpu_vu_generated_private_state.tpc :
				(VU1.VI[REG_TPC].UL << 3),
			commit_private_state ? 1u : 0u,
			static_cast<unsigned long long>(wait_polls));
	}
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (initial_pending != 0u)
	{
		PublishGpuVuHealth(
			static_cast<u32>(GpuVuHealthCpu1Stage::DrainComplete),
			static_cast<u32>(std::min<u64>(wait_polls,
				std::numeric_limits<u32>::max())));
	}
#endif
	return true;
#else
	(void)commit_private_state;
	const u32 initial_pending = GeneratedLoopKernelPendingExecutionCount();
	const u64 drain_count = wait_for_all && initial_pending != 0u ?
		++m_gpu_vu_generated_batch_drain_count : 0u;
	const bool report_drain = drain_count != 0u &&
		(drain_count <= 8u || (drain_count & (drain_count - 1u)) == 0u);
	if (report_drain)
	{
		Console.WriteLn(
			"GPU-VU generated_batch_drain=begin drain_count=%llu pending=%u "
			"enqueued=%llu completed=%llu private_state=%u.",
			static_cast<unsigned long long>(drain_count), initial_pending,
			static_cast<unsigned long long>(
				m_execute_jobs_enqueued.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(
				m_execute_jobs_completed.load(std::memory_order_acquire)),
			m_gpu_vu_generated_private_state.valid ? 1u : 0u);
	}
	if (wait_for_all && !m_gpu_vu_generated_pending_transactions.empty())
	{
		const Common::Timer::Value drain_wait_started =
			VitaPerformanceTelemetry::IsEnabled() ?
				Common::Timer::GetCurrentValue() : 0;
		// Publish the whole compatible descriptor run before waiting.  This is
		// one coarse mailbox/GXM boundary for the batch, not one flush per
		// Execute.
		VitaGS::FlushMtvuPath1Completions();
		m_gpu_vu_generated_unpublished_transactions = 0;
		u64 wait_polls = 0u;
		for (;;)
		{
			u32 prepared = 0u;
			u32 completed = 0u;
			u32 failed = 0u;
			u64 first_prepared_sequence = 0u;
			for (const auto& transaction :
				m_gpu_vu_generated_pending_transactions)
			{
				if (!transaction)
				{
					failed++;
					continue;
				}
				switch (transaction->Stage())
				{
					case VitaGpuVu::GeneratedLoopKernelTransactionStage::Prepared:
					case VitaGpuVu::GeneratedLoopKernelTransactionStage::GsAccepted:
						prepared++;
						if (first_prepared_sequence == 0u)
							first_prepared_sequence = transaction->Sequence();
						break;
					case VitaGpuVu::GeneratedLoopKernelTransactionStage::GpuCompleted:
						completed++;
						break;
					case VitaGpuVu::GeneratedLoopKernelTransactionStage::Failed:
						failed++;
						break;
					case VitaGpuVu::GeneratedLoopKernelTransactionStage::Adopted:
						failed++;
						break;
				}
			}
			if (prepared == 0u)
				break;

			// A pressure/observer drain is already a coarse wait. Re-wake MTGS
			// while sleeping so a scene-end notification followed by a second
			// mid-scene slot cannot lose the hand-off which polls that later slot.
			// This is one poll per whole batch, never one wait per Execute/XGKICK.
			VitaGS::NotifyUniversalGpuVuProgress();
			Threading::Sleep(1);
			wait_polls++;
			if (wait_polls == 100u ||
				(wait_polls > 100u &&
				 (wait_polls & (wait_polls - 1u)) == 0u))
			{
				Console.WriteLn(
					"GPU-VU generated_batch_wait=poll drain_count=%llu "
					"polls=%llu prepared=%u completed=%u failed=%u "
					"first_prepared_seq=%llu pending=%u.",
					static_cast<unsigned long long>(drain_count),
					static_cast<unsigned long long>(wait_polls), prepared,
					completed, failed,
					static_cast<unsigned long long>(first_prepared_sequence),
					initial_pending);
			}
		}
		if (drain_wait_started != 0)
		{
			VitaGpuVu::RecordGeneratedLoopKernelBatchDrainWait(
				MtvuElapsedTelemetryMicroseconds(drain_wait_started),
				wait_polls);
		}
		if (AdoptGeneratedLoopKernelTransactionBatch())
		{
			if (report_drain)
			{
				Console.WriteLn(
					"GPU-VU generated_batch_drain=complete drain_count=%llu "
					"pending=0 enqueued=%llu completed=%llu canonical_tpc=%04x "
					"private_tpc=%04x bridge_quarantined=%u commit=coalesced.",
					static_cast<unsigned long long>(drain_count),
					static_cast<unsigned long long>(
						m_execute_jobs_enqueued.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(
						m_execute_jobs_completed.load(std::memory_order_acquire)),
					VU1.VI[REG_TPC].UL << 3, VU1.VI[REG_TPC].UL << 3,
					m_gpu_vu_private_state_bridge_quarantined ? 1u : 0u);
			}
			return true;
		}
	}
	for (;;)
	{
		if (m_gpu_vu_generated_pending_transactions.empty())
		{
			if (!m_gpu_vu_generated_private_continuations.empty())
			{
				Console.Error(
					"GPU-VU: private state-load continuation lost its GPU predecessor.");
				return false;
			}
			if (report_drain)
			{
				Console.WriteLn(
					"GPU-VU generated_batch_drain=complete drain_count=%llu "
					"pending=0 enqueued=%llu completed=%llu canonical_tpc=%04x "
					"private_tpc=%04x bridge_quarantined=%u.",
					static_cast<unsigned long long>(drain_count),
					static_cast<unsigned long long>(
						m_execute_jobs_enqueued.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(
						m_execute_jobs_completed.load(std::memory_order_acquire)),
					VU1.VI[REG_TPC].UL << 3,
					m_gpu_vu_generated_private_state.tpc,
					m_gpu_vu_private_state_bridge_quarantined ? 1u : 0u);
			}
			InvalidateGeneratedLoopKernelPrivateState();
			return true;
		}

		const std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>&
			transaction = m_gpu_vu_generated_pending_transactions.front();
		if (!transaction)
			return false;
		auto stage = transaction->Stage();
		if (stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::Prepared ||
			stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::GsAccepted)
		{
			if (!wait_for_all)
				return true;
			transaction->WaitForTerminal();
			stage = transaction->Stage();
		}
		if (stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::Failed)
		{
			Console.Error(
				"GPU-VU seq=%llu provider=generated-loop-kernel "
				"transaction_commit=FAILED stage=%s failure=%s detail=%s; "
				"refusing CPU replay after a published direct-output transaction.",
				static_cast<unsigned long long>(transaction->Sequence()),
				VitaGpuVu::GeneratedLoopKernelTransactionStageName(stage),
				VitaGpuVu::GeneratedLoopKernelTransactionFailureName(
					transaction->Failure()),
				transaction->FailureDetail() ? transaction->FailureDetail() : "none");
			return false;
		}
		if (stage !=
				VitaGpuVu::GeneratedLoopKernelTransactionStage::GpuCompleted)
		{
			Console.Error(
				"GPU-VU seq=%llu reached invalid generated transaction "
				"stage=%s failure=%s detail=%s.",
				static_cast<unsigned long long>(transaction->Sequence()),
				VitaGpuVu::GeneratedLoopKernelTransactionStageName(stage),
				VitaGpuVu::GeneratedLoopKernelTransactionFailureName(
					transaction->Failure()),
				transaction->FailureDetail() ? transaction->FailureDetail() : "none");
			return false;
		}

		std::vector<VitaGpuVu::VifUnpackSpan> replay_unpacks =
			transaction->TakeReplayUnpacks();
		ReplayVifUnpackSpans(&replay_unpacks);

		const auto& targets = transaction->StoreTargets();
		const auto& gpu_words = transaction->OutputWords();
		const auto& commit_words = transaction->CommittedStoreWords();
		const auto& pre_loop_targets = transaction->PreLoopStoreTargets();
		const auto& pre_loop_words = transaction->PreLoopStoreWords();
		if (targets.empty() ||
			gpu_words.size() != transaction->OutputWordCount() ||
			commit_words.size() != transaction->OutputWordCount() ||
			commit_words.size() != transaction->StoreEntryCount() * 4u ||
			pre_loop_words.size() !=
				transaction->PreLoopStoreEntryCount() * 4u)
		{
			Console.Error(
				"GPU-VU seq=%llu generated successor payload changed shape.",
				static_cast<unsigned long long>(transaction->Sequence()));
			return false;
		}
		u32* const vu_memory = reinterpret_cast<u32*>(VU1.Mem);
		// VIF UNPACKs precede Execute, then the PairPlan entry slice precedes the
		// parallel loop body.  Preserve that architectural order when publishing
		// the private successor generation.
		for (u32 entry = 0u; entry < pre_loop_targets.size(); entry++)
		{
			const VitaGpuVu::GeneratedLoopKernelStoreTarget& target =
				pre_loop_targets[entry];
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) != 0u)
					vu_memory[target.address_qword * 4u + lane] =
						pre_loop_words[entry * 4u + lane];
			}
		}
		for (u32 entry = 0u; entry < targets.size(); entry++)
		{
			const VitaGpuVu::GeneratedLoopKernelStoreTarget& target =
				targets[entry];
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((target.lane_mask & (0x8u >> lane)) != 0u)
					vu_memory[target.address_qword * 4u + lane] =
						commit_words[entry * 4u + lane];
			}
		}
		for (u16 qword : transaction->AdcPatchQwords())
			vu_memory[static_cast<u32>(qword) * 4u + 3u] = 0x00008000u;

		const auto& vf_lanes = transaction->FinalVfLanes();
		const auto& final_vf = transaction->FinalVfValues();
		for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
		{
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((vf_lanes[reg] & (0x8u >> lane)) != 0u)
					VU1.VF[reg].UL[lane] = final_vf[reg][lane];
			}
		}
		const auto& final_acc = transaction->FinalAccValues();
		for (u32 lane = 0u; lane < 4u; lane++)
		{
			if ((transaction->FinalAccLanes() & (0x8u >> lane)) != 0u)
				VU1.ACC.UL[lane] = final_acc[lane];
		}
		if (transaction->FinalQ())
			VU1.VI[REG_Q].UL = transaction->FinalQValue();
		if (transaction->FinalP())
			VU1.VI[REG_P].UL = transaction->FinalPValue();
		if (transaction->FinalI())
			VU1.VI[REG_I].UL = transaction->FinalIValue();
		const auto& final_vi = transaction->FinalViValues();
		for (u32 reg = 1u; reg < final_vi.size(); reg++)
		{
			if ((transaction->FinalViWriteMask() & (1u << reg)) != 0u)
				VU1.VI[reg].US[0] = final_vi[reg];
		}
		VU1.VI[0].UL = 0u;
		VU1.VI[REG_TPC].UL = transaction->UniqueResumePc() >> 3;

		BeginProgram();
		EndProgram(InterruptFlagVUEBit);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
		Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
		vuCycles[vuCycleIdx].store(4u, std::memory_order_release);
		vuCycleIdx = (vuCycleIdx + 1) & 3;
		if (!transaction->MarkAdopted())
			return false;
		VitaGpuVu::RecordGeneratedLoopKernelAccepted(
			transaction->ExecutedPairs());
		// Logical epoch completion and physical scene retirement are different
		// quantities: many transactions normally share one EndScene notification.
		VitaGpuVu::RecordUniversalGpuVuProviderCompletion(true, 0u, 0u);
		const u64 adopted_sequence = transaction->Sequence();
		if (adopted_sequence <= 8u ||
			(adopted_sequence & (adopted_sequence - 1u)) == 0u)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-loop-kernel accepted=1 "
				"product_accepted=1 generated_epochs=1 pairs=%u "
				"output=direct-tfx output_profile=%s state=transactional "
				"canonical_store_journal=%s pre_loop_stores=%u "
				"transaction_commit=adopted "
				"cpu_vu_calls=0 cpu_vu_pairs=0 validation_canary=%u.",
				static_cast<unsigned long long>(adopted_sequence),
				transaction->ExecutedPairs(),
				VitaGpuVu::GeneratedLoopKernelNumericProfileName(
					transaction->OutputNumericProfile()),
				transaction->StoreCommitModeName(),
				transaction->PreLoopStoreEntryCount(),
				transaction->IsValidationCanary() ? 1u : 0u);
		}

		m_gpu_vu_generated_pending_transactions.pop_front();
		if (!ApplyGeneratedPrivateStateLoadContinuations(adopted_sequence))
			return false;
		PublishGeneratedLoopKernelPendingExecutionCount();
	}
#endif
}

void VU_Thread::ExecuteRingBuffer()
{
	Threading::SetNameOfCurrentThread("MTVU");
	u32 primed_direct_entry_token = 0;
	u32 primed_direct_resume_token = 0;
	VitaGpuVu::DirectContinuationSeed active_direct_continuation;
	VitaGpuVu::DirectContinuationSeed pending_direct_continuation;
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	bool reported_first_direct_job = false;
	bool reported_first_queued_direct_draw = false;
#endif
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	u64 universal_fallback_count = 0;
	u64 generated_loop_kernel_preflight_fallback_count = 0;
	u64 generated_loop_kernel_dispatch_rejection_count = 0;
	u64 generated_architecture_quarantine_fallback_count = 0;
	const auto report_universal_fallback = [&universal_fallback_count]() {
		const u64 count = ++universal_fallback_count;
		return count <= 8 || (count & (count - 1)) == 0;
	};
#endif

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	PublishGpuVuHealth(static_cast<u32>(GpuVuHealthCpu1Stage::Started));
#endif

	for (;;)
	{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuHealth(static_cast<u32>(GpuVuHealthCpu1Stage::BeforeWait));
#endif
		semaEvent.WaitForWork();
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuHealth(static_cast<u32>(GpuVuHealthCpu1Stage::AfterWake));
#endif
		if (m_shutdown_flag.load(std::memory_order_acquire))
			break;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuHealth(
			static_cast<u32>(GpuVuHealthCpu1Stage::RetirementPoll));
#endif
		const bool retirement_poll_ok = DrainUniversalGpuVuEpochs(false);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuHealth(
			static_cast<u32>(GpuVuHealthCpu1Stage::RetirementPollReturned));
#endif
		pxAssertRel(retirement_poll_ok,
			"universal GPU-VU completion could not be adopted");

		while (m_ato_read_pos.load(std::memory_order_relaxed) != GetWritePos())
		{
			u32 tag = Read();
			const Common::Timer::Value mtvu_record_started =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			const bool completes_vu_execution =
				tag == MTVU_VU_EXECUTE || tag == MTVU_VU_EXECUTE_DIRECT ||
				tag == MTVU_VU_EXECUTE_DIRECT_PRIME;
			bool defer_vu_execution_completion = false;
			switch (tag)
			{
				case MTVU_VU_EXECUTE:
				case MTVU_VU_EXECUTE_DIRECT:
				case MTVU_VU_EXECUTE_DIRECT_PRIME:
				{
					const bool retirement_poll_ok =
						DrainGeneratedLoopKernelTransactions(false, false);
					pxAssertRel(retirement_poll_ok,
						"VU execute could not poll its generated GPU predecessor");
					if (!m_gpu_vu_generated_pending_transactions.empty() &&
						(!m_gpu_vu_generated_private_state.valid ||
					 GeneratedLoopKernelPhysicalTransactionCount() >=
							 MaximumGeneratedLoopKernelInFlightExecutions))
					{
						pxAssertRel(DrainGeneratedLoopKernelTransactions(true, false),
							"generated GPU-VU microbatch could not retire at its bound");
					}
					const bool generated_private_predecessor =
						m_gpu_vu_generated_private_state.valid;
					const bool has_direct_program =
						tag == MTVU_VU_EXECUTE_DIRECT ||
						tag == MTVU_VU_EXECUTE_DIRECT_PRIME;
					const bool allow_direct_draw =
						tag == MTVU_VU_EXECUTE_DIRECT;
					VU1.cycle = 0;
					s32 addr = Read();
					vifRegs.top = Read();
					vifRegs.itop = Read();
						const u32 execution_vif_top = vifRegs.top;
						const u32 execution_vif_itop = vifRegs.itop;
						vuFBRST = Read();
						const u64 execute_enqueued_at =
							static_cast<u64>(Read()) |
							(static_cast<u64>(Read()) << 32);
						if (VitaPerformanceTelemetry::IsEnabled() &&
							execute_enqueued_at != 0)
						{
							VitaGpuVu::RecordUniversalGpuVuMtvuExecuteQueueAge(
								MtvuElapsedTelemetryMicroseconds(execute_enqueued_at));
						}
						const VitaGpuVu::DirectProgramToken direct_program{
						has_direct_program ? Read() : 0};
					const VitaGpuVu::DirectProgramToken continuation_program{
						has_direct_program ? Read() : 0};
					u64 generated_cold_program_identity = 0;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
					// MSCNT has no explicit entry and therefore cannot be classified
					// against an unresolved predecessor TPC. A bounded queue also drains
					// before taking a fifth private generation; ordinary explicit MSCAL
					// roots remain free to queue behind the prior sequence.
					if ((addr == -1 ||
						m_gpu_vu_universal_pending_epochs.size() >=
							MaximumOutstandingVuExecutions) &&
						!m_gpu_vu_universal_pending_epochs.empty())
					{
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"universal GPU-VU predecessor could not be drained");
					}
						std::unique_ptr<VitaGpuVu::UniversalGpuVuEpoch>
							universal_epoch;
						std::unique_ptr<VitaGpuVu::GpuVuDraw>
							pending_loop_kernel_shadow_draw;
						u32 pending_loop_kernel_shadow_pairs = 0;
						bool pending_loop_kernel_shadow_validation_canary = false;
						VitaGpuVu::UniversalGpuVuRejection universal_rejection =
						VitaGpuVu::UniversalGpuVuRejection::None;
					u32 universal_pair_count = 0;
					u32 universal_dynamic_pair_bound = 0;
						bool universal_was_queued = false;
						const bool legacy_universal_device_available =
							VitaGpuVu::IsUniversalGpuVuDeviceAvailable();
						const bool generated_loop_kernel_product_enabled =
							VitaGpuVu::IsGeneratedLoopKernelProductAdmissionEnabled();
						const bool universal_device_available =
							legacy_universal_device_available &&
							generated_loop_kernel_product_enabled;
						if (tag == MTVU_VU_EXECUTE &&
							!generated_loop_kernel_product_enabled)
						{
							VitaGpuVu::RecordUniversalGpuVuProductPolicyCpuFallback(
								VitaGpuVu::UniversalGpuVuRejection::
									GeneratedArchitectureQuarantined);
							const u64 count =
								++generated_architecture_quarantine_fallback_count;
							if (count <= 8 || (count & (count - 1)) == 0)
							{
								Console.WriteLn(
									"GPU-VU seq=0 provider=cpu-mtvu accepted=0 "
									"pre_effect=1 reason=generated-architecture-quarantined "
									"pairs=0 pair_count_valid=0 gpu_jobs=0 cpu_fallback=1 "
									"fallback_count=%llu",
									static_cast<unsigned long long>(count));
							}
						}
					std::vector<BufferedGeneratedExecute>
						buffered_universal_executes;
					s32 buffered_universal_end_pos = m_read_pos;
					u32 consumed_universal_executes = 0;
					Common::Timer::Value universal_attempt_start = 0;
						const u32 universal_configuration_bits =
							universal_device_available ?
								VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits() : 0;
					const u64 universal_predecessor_sequence =
						generated_private_predecessor ?
							(!m_gpu_vu_generated_private_continuations.empty() ?
								m_gpu_vu_generated_private_continuations.back().sequence :
								m_gpu_vu_generated_pending_transactions.back()->Sequence()) :
						m_gpu_vu_universal_pending_epochs.empty() ?
							m_gpu_vu_universal_committed_sequence :
							m_gpu_vu_universal_pending_epochs.back().epoch->Sequence();
					const u32 universal_entry_pc = addr == -1 ?
						(generated_private_predecessor ?
							m_gpu_vu_generated_private_state.tpc :
							(m_gpu_vu_universal_committed_sequence != 0 ?
								m_gpu_vu_universal_committed_tpc :
								(VU1.VI[REG_TPC].UL << 3))) :
						((static_cast<u32>(addr) & 0x7ffu) << 3);
					const auto find_universal_dispatch_cache =
						[&](u32 entry_pc, u32 top, u32 itop,
							u32 fbrst) -> UniversalDispatchCostCacheEntry* {
							if (!universal_device_available)
								return nullptr;
							for (UniversalDispatchCostCacheEntry& cached :
								m_gpu_vu_dispatch_cost_cache)
							{
								if (cached.valid &&
									cached.micro_generation == m_gpu_vu_micro_generation &&
									cached.entry_pc == entry_pc &&
									cached.configuration_bits ==
										universal_configuration_bits &&
									cached.vif_top == top && cached.vif_itop == itop &&
									cached.observer_fbrst == (fbrst & 0xc00u))
								{
									return &cached;
								}
							}
							return nullptr;
						};
					UniversalDispatchCostCacheEntry* universal_dispatch_cache =
						find_universal_dispatch_cache(universal_entry_pc,
							execution_vif_top, execution_vif_itop, vuFBRST);
					if (tag == MTVU_VU_EXECUTE)
					{
						VitaGpuVu::RecordUniversalGpuVuMtvuDispatchCacheLookup(
							universal_dispatch_cache != nullptr);
					}
#endif
					#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						if (universal_device_available && tag == MTVU_VU_EXECUTE &&
							universal_dispatch_cache &&
						universal_dispatch_cache->allow_multi_execute_gather)
					{
						// The producer publishes a complete VIF transfer with one
						// release store. Look ahead only through pointer-free captured
						// UNPACKs and ordinary Execute records; every other MTVU event
						// is an architectural ownership boundary. Nothing is consumed
						// until complete-epoch preflight and queue publication succeed.
						const Common::Timer::Value gather_started =
							VitaPerformanceTelemetry::IsEnabled() ?
								Common::Timer::GetCurrentValue() : 0;
						s32 cursor = m_read_pos;
						const s32 published_end = GetWritePos();
						std::vector<VitaGpuVu::VifUnpackSpan> pending_unpacks;
						pending_unpacks.reserve(8);
						while (cursor != published_end &&
							buffered_universal_executes.size() <
								MaximumUniversalGpuVuSpeculativeExecuteGather)
						{
							const u32 following_tag = buffer[cursor++];
							if (following_tag == MTVU_NULL_PACKET)
							{
								cursor = 0;
								continue;
							}
							if (following_tag == MTVU_VIF_UNPACK_CAPTURED)
							{
								VitaGpuVu::VifUnpackSpan span;
								std::memcpy(&span, &buffer[cursor], sizeof(span));
								cursor += size_u32(sizeof(span));
								pending_unpacks.push_back(span);
								continue;
							}
							if (following_tag != MTVU_VU_EXECUTE)
								break;

							BufferedGeneratedExecute execute;
							execute.addr = static_cast<s32>(buffer[cursor++]);
							execute.vif_top = buffer[cursor++];
							execute.vif_itop = buffer[cursor++];
							execute.fbrst = buffer[cursor++];
							execute.enqueued_at =
								static_cast<u64>(buffer[cursor++]);
							execute.enqueued_at |=
								static_cast<u64>(buffer[cursor++]) << 32;
						execute.unpacks = std::move(pending_unpacks);
						pending_unpacks.clear();
						execute.end_pos = cursor;
						buffered_universal_executes.push_back(
								std::move(execute));
							buffered_universal_end_pos = cursor;
						}
						if (gather_started != 0)
						{
							VitaGpuVu::RecordGeneratedLoopKernelGather(
								static_cast<u32>(buffered_universal_executes.size()),
								MtvuElapsedTelemetryMicroseconds(gather_started));
						}
					}
						else if (universal_device_available &&
							tag == MTVU_VU_EXECUTE &&
						m_read_pos != GetWritePos())
					{
						// Unknown and rejected entries must pay at most one single-entry
						// proof. In particular, do not repeatedly analyze a sliding MTVU
						// lookahead window merely to discover the same pre-effect fallback.
						VitaGpuVu::
							RecordUniversalGpuVuMtvuMultiExecuteGatherSuppressed();
					}
					if (universal_device_available && tag == MTVU_VU_EXECUTE &&
						universal_dispatch_cache &&
						!buffered_universal_executes.empty() &&
						(universal_dispatch_cache->generated_product_ready ||
						 (generated_private_predecessor &&
						  universal_dispatch_cache->generated_state_formula_ready)))
					{
						// This is the product hot boundary. An attested source/configuration
						// generation already has every static proof and compiled GXP needed
						// by the chain. Do not enter the specialized provider, construct a
						// UniversalGpuVuEpochBuildRequest, or revisit cold admission.
						if (addr != -1)
							active_direct_continuation = {};
						if (TryQueueAttestedGeneratedProductHotChain(
								*universal_dispatch_cache, universal_entry_pc,
								addr == -1,
								static_cast<u16>(execution_vif_top),
								static_cast<u16>(execution_vif_itop),
								&m_deferred_vif_unpacks,
								&buffered_universal_executes))
						{
							defer_vu_execution_completion = true;
							break;
						}
					}
					#endif
					if (addr != -1)
						active_direct_continuation = {};
					if (!generated_private_predecessor && addr != -1)
						VU1.VI[REG_TPC].UL = addr & 0x7FF;
					if (!generated_private_predecessor)
						CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
					// Under MTVU this worker, not vu1ExecMicro() on CPU0, owns the
					// architectural start/completion pair. Keep both notifications on
					// this side of the queue so multiple pending MSCAL/MSCNT jobs cannot
					// collapse into one armed boolean.
					Pcsx2Trace::NotifyMachineCheckpointVu1ProgramStarted();
#endif
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
					// Bounded arrival evidence for the first direct job only.
					if (has_direct_program && !reported_first_direct_job)
					{
						reported_first_direct_job = true;
						Console.WriteLn(
							"GPU-VU: first direct VU1 job token %08x, TPC "
							"%04x, TOP %04x, ITOP %04x, %u deferred spans, "
							"connected %u.",
							direct_program.value, VU1.VI[REG_TPC].UL << 3,
							vifRegs.top, vifRegs.itop,
							static_cast<u32>(m_deferred_vif_unpacks.size()),
							VitaGpuVu::IsDirectDrawAdmissionConnected() ? 1u :
																		  0u);
					}
#endif
					bool queued_direct_draw = false;
					if (!allow_direct_draw)
					{
						// Prime-only jobs are an intentional CPU execution, not
						// a rejected admission attempt.
					}
					else if (!VitaGpuVu::IsDirectDrawAdmissionConnected())
					{
						VitaGpuVu::RecordDirectAdmissionFailure(
							VitaGpuVu::AdmissionFailure::Disconnected);
					}
					else if (!direct_program.IsValid())
					{
						VitaGpuVu::RecordDirectAdmissionFailure(
							VitaGpuVu::AdmissionFailure::NoProgramToken);
					}
					if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
						direct_program.IsValid() &&
						m_gpu_vu_universal_committed_sequence == 0 &&
						m_gpu_vu_universal_pending_epochs.empty() &&
						m_gpu_vu_generated_pending_transactions.empty())
					{
						std::array<u16, 16> initial_vi{};
						for (u32 reg = 0; reg < initial_vi.size(); reg++)
							initial_vi[reg] = VU1.VI[reg].US[0];

						static thread_local GpuVuMemoryView::BulkWorkspace memory_workspace;
						memory_workspace.Begin();
						GpuVuMemoryView memory_view{
							VU1.Mem, &m_deferred_vif_unpacks, nullptr,
								&memory_workspace, VU1.Mem, nullptr, nullptr};
						VitaGpuVu::InvocationEvaluationContext context;
						context.vif_top = static_cast<u16>(vifRegs.top);
						context.vif_itop = static_cast<u16>(vifRegs.itop);
						context.initial_vi = initial_vi.data();
						context.initial_vf_words = &VU1.VF[0].UL[0];
						context.initial_acc_words = &VU1.ACC.UL[0];
						context.initial_q = VU1.VI[REG_Q].UL;
						context.initial_p = VU1.VI[REG_P].UL;
						context.initial_i = VU1.VI[REG_I].UL;
						context.memory_user = &memory_view;
						context.read_memory_u16 = ReadGpuVuMemoryU16;
						context.read_memory_u32 = ReadGpuVuMemoryU32;
						context.read_memory_qwords = ReadGpuVuMemoryQwords;
							context.memory_qwords_have_canonical_owner =
								GpuVuMemoryQwordsHaveCanonicalOwner;
							context.bind_memory_qwords_to_raw_payload =
								BindGpuVuMemoryRawQwords;
						u32& primed_direct_program_token =
							addr == -1 ? primed_direct_resume_token :
								primed_direct_entry_token;
						if (direct_program.value !=
								primed_direct_program_token &&
							VitaGpuVu::PrimeDirectProgram(
								direct_program, context))
						{
							primed_direct_program_token =
								direct_program.value;
						}
						bool begins_continuation = false;
						std::unique_ptr<VitaGpuVu::GpuVuDraw> draw;
						if (allow_direct_draw && addr != -1 &&
							continuation_program.IsValid() &&
							VitaGpuVu::IsDirectContinuationPair(
								direct_program, continuation_program))
						{
							pending_direct_continuation = {};
							draw = VitaGpuVu::BuildDirectGpuVuDraw(
								direct_program, context,
								m_deferred_vif_unpacks);
							begins_continuation = draw &&
								VitaGpuVu::CaptureDirectContinuationSeed(
									direct_program, continuation_program,
									context, *draw,
									&pending_direct_continuation);
							if (!begins_continuation)
								draw.reset();
						}
						else if (allow_direct_draw && addr == -1 &&
							active_direct_continuation.IsValid() &&
							active_direct_continuation.entry_program ==
								continuation_program &&
							active_direct_continuation.resume_program ==
								direct_program)
						{
							draw =
								VitaGpuVu::BuildDirectGpuVuContinuationDraw(
									active_direct_continuation, context,
									m_deferred_vif_unpacks);
						}
						else if (allow_direct_draw)
						{
							draw = VitaGpuVu::BuildDirectGpuVuDraw(
								direct_program, context,
								m_deferred_vif_unpacks);
						}
						const u32 direct_vertices =
							draw ? draw->vertex_count : 0;
						const u32 direct_primitives =
							draw ? draw->primitive_count : 0;
						const u32 direct_streams =
							draw ? static_cast<u32>(draw->streams.size()) : 0;
						const std::array<u16, 16> direct_final_vi =
							draw ? draw->final_vi_values :
								   std::array<u16, 16>{};
						const u32 direct_final_vi_mask =
							draw ? draw->final_vi_write_mask : 0;
						const bool built = static_cast<bool>(draw);
						if (allow_direct_draw && !built)
						{
							VitaGpuVu::RecordDirectAdmissionFailure(
								VitaGpuVu::AdmissionFailure::BuildFailed);
						}
						if (built &&
							VitaGS::QueueGpuVuDraw(std::move(draw)))
						{
	#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
								if (!reported_first_queued_direct_draw)
								{
								reported_first_queued_direct_draw = true;
								Console.WriteLn(
									"GPU-VU: first direct draw queued from MTVU "
									"(%u vertices, %u primitives, %u raw streams); "
									"CpuVU1->Execute bypassed.",
									direct_vertices, direct_primitives,
									direct_streams);
							}
#endif
							VitaGpuVu::DirectProgramInfo info;
							if (VitaGpuVu::GetDirectProgramInfo(
									direct_program, &info) &&
								info.resume_pc_count == 1)
							{
								VU1.VI[REG_TPC].UL =
									info.unique_resume_pc >> 3;
							}
							// Keep VIF writes in the immutable overlay. The draw
							// retained each payload it references, and a later CPU
							// fallback or architectural observer materializes the
							// remaining journal before reading VU memory.
							for (u32 reg = 1;
								 reg < direct_final_vi.size(); reg++)
							{
								if ((direct_final_vi_mask &
										(1u << reg)) != 0)
								{
									VU1.VI[reg].US[0] =
										direct_final_vi[reg];
								}
							}
							BeginProgram();
							EndProgram(InterruptFlagVUEBit);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
							Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
							vuCycles[vuCycleIdx].store(
								4, std::memory_order_release);
							vuCycleIdx = (vuCycleIdx + 1) & 3;
							if (begins_continuation)
								active_direct_continuation =
									std::move(
										pending_direct_continuation);
							queued_direct_draw = true;
						}
						else if (built)
						{
							VitaGpuVu::RecordDirectAdmissionFailure(
								VitaGpuVu::AdmissionFailure::QueueRejected);
						}
					}
					if (queued_direct_draw)
						break;

	#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
							if (universal_device_available)
							{
							universal_attempt_start =
								VitaPerformanceTelemetry::IsEnabled() ?
									Common::Timer::GetCurrentValue() : 0;
							VitaGpuVu::UniversalGpuVuEpochBuildRequest request;
						request.micro = VU1.Micro;
						request.micro_size = VU1_PROGSIZE;
						request.start_pc = addr == -1 ? 0u :
							(static_cast<u32>(addr) & 0x7ffu) << 3;
						request.current_tpc =
							generated_private_predecessor ?
								m_gpu_vu_generated_private_state.tpc :
							m_gpu_vu_universal_committed_sequence != 0 ?
								m_gpu_vu_universal_committed_tpc :
								(VU1.VI[REG_TPC].UL << 3);
						request.configuration_bits = universal_configuration_bits;
						request.fbrst = vuFBRST;
					request.predecessor_sequence =
						universal_predecessor_sequence;
						request.vif_top = static_cast<u16>(execution_vif_top);
						request.vif_itop = static_cast<u16>(execution_vif_itop);
						for (u32 lane = 0; lane < 4; lane++)
						{
							request.vif_row[lane] = vif.MaskRow._u32[lane];
							request.vif_column[lane] = vif.MaskCol._u32[lane];
						}
						for (u32 reg = 0; reg < request.initial_vi.size(); reg++)
						{
							request.initial_vi[reg] = generated_private_predecessor ?
								m_gpu_vu_generated_private_state.vi[reg] :
								VU1.VI[reg].US[0];
						}
						request.resume = addr == -1;
						request.unpacks = &m_deferred_vif_unpacks;
						std::string universal_error;
						std::vector<
							VitaGpuVu::UniversalGpuVuAdditionalExecuteRequest>
							additional_executes;
						additional_executes.reserve(
							buffered_universal_executes.size());
						u32 preceding_entry_pc = universal_entry_pc;
						bool complete_execute_chain = true;
						if (!universal_dispatch_cache ||
							!universal_dispatch_cache->generated_product_ready)
						{
							for (const BufferedGeneratedExecute& buffered :
								 buffered_universal_executes)
							{
								VitaGpuVu::UniversalGpuVuAdditionalExecuteRequest
									execute;
								execute.maximum_pairs =
									VitaGpuVu::UniversalCommandEpochMaximumPairsPerExecute;
								execute.fbrst = buffered.fbrst;
								execute.vif_top =
									static_cast<u16>(buffered.vif_top);
								execute.vif_itop =
									static_cast<u16>(buffered.vif_itop);
								execute.resume = buffered.addr == -1;
								execute.unpacks = &buffered.unpacks;
								if (execute.resume)
								{
									VitaGpuVu::ProgramAnalysis preceding_analysis;
									if (!VitaGpuVu::AnalyzeGpuVu1ProgramForConfiguration(
											VU1.Micro, VU1_PROGSIZE,
											preceding_entry_pc,
											(request.configuration_bits &
											 VitaGpuVu::UniversalConfigurationAssumeScheduled) != 0,
											(request.configuration_bits &
											 VitaGpuVu::UniversalConfigurationInstantQp) != 0,
											&preceding_analysis, &universal_error) ||
										preceding_analysis.resume_pcs.size() != 1)
									{
										complete_execute_chain = false;
										break;
									}
									execute.current_tpc =
										preceding_analysis.resume_pcs.front();
									preceding_entry_pc = execute.current_tpc;
								}
								else
								{
									execute.start_pc =
										(static_cast<u32>(buffered.addr) & 0x7ffu) << 3;
									preceding_entry_pc = execute.start_pc;
								}
								additional_executes.push_back(execute);
							}
						}
						if (!complete_execute_chain)
						{
							// A continuation with more than one possible post-E TPC needs
							// path-set preflight. Leave every looked-ahead record untouched;
							// the ordinary worker path executes them in original order.
							additional_executes.clear();
							buffered_universal_executes.clear();
							buffered_universal_end_pos = m_read_pos;
						}
						if (!additional_executes.empty())
							request.additional_executes = &additional_executes;
						auto update_dispatch_cache =
							[&](bool allow_gather, bool skip_single,
								VitaGpuVu::UniversalGpuVuRejection cached_rejection,
								u32 cached_pairs, u32 cached_dynamic_bound)
						{
								if (!universal_dispatch_cache)
								{
									universal_dispatch_cache =
										&m_gpu_vu_dispatch_cost_cache[
											m_gpu_vu_dispatch_cost_cache_next++ %
											m_gpu_vu_dispatch_cost_cache.size()];
								}
								universal_dispatch_cache->valid = true;
								universal_dispatch_cache->allow_multi_execute_gather =
									allow_gather;
								universal_dispatch_cache->skip_single_attempt = skip_single;
								universal_dispatch_cache->wait_for_generated_program = false;
								universal_dispatch_cache->wait_for_generated_compiler_idle =
									false;
								universal_dispatch_cache->generated_terminally_unavailable =
									false;
									universal_dispatch_cache->generated_product_ready = false;
									universal_dispatch_cache->generated_state_formula_ready = false;
								universal_dispatch_cache->micro_generation =
									m_gpu_vu_micro_generation;
								universal_dispatch_cache->generated_program_identity = 0;
								universal_dispatch_cache->generated_key_low = 0;
									universal_dispatch_cache->generated_key_high = 0;
									universal_dispatch_cache->generated_attestation_key_low = 0;
									universal_dispatch_cache->generated_attestation_key_high = 0;
									universal_dispatch_cache->generated_compiler_idle_generation =
										0;
									universal_dispatch_cache->wait_for_generated_attestation = false;
									universal_dispatch_cache->generated_loop_kernel_abi = 0;
									universal_dispatch_cache->generated_semantic_profile_key = 0;
								universal_dispatch_cache->entry_pc = universal_entry_pc;
								universal_dispatch_cache->configuration_bits =
									request.configuration_bits;
								universal_dispatch_cache->vif_top = request.vif_top;
								universal_dispatch_cache->vif_itop = request.vif_itop;
								universal_dispatch_cache->observer_fbrst =
									request.fbrst & 0xc00u;
								universal_dispatch_cache->rejection =
									static_cast<u32>(cached_rejection);
								universal_dispatch_cache->pair_count = cached_pairs;
									universal_dispatch_cache->dynamic_pair_upper_bound =
										cached_dynamic_bound;
									universal_dispatch_cache->generated_state_formula = {};
								};
							// An already-proven generated entry or state formula must never fall
							// back into the legacy multi-Execute epoch builder. Consume the
							// pointer-free lookahead directly through the transactional owner.
							// Each complete E-bit Execute is queued before its successor is
							// examined, so a later dynamic guard failure leaves the accepted
							// prefix authoritative and the untouched suffix in the MTVU ring.
							const Common::Timer::Value generated_chain_dispatch_started =
								VitaPerformanceTelemetry::IsEnabled() ?
									Common::Timer::GetCurrentValue() : 0;
							if (!buffered_universal_executes.empty() &&
								universal_dispatch_cache &&
								(universal_dispatch_cache->generated_product_ready ||
								 (generated_private_predecessor &&
								  universal_dispatch_cache->generated_state_formula_ready)))
							{
								GeneratedProductHotQueueResult base_result{};
								std::string hot_error;
								u64 base_formula_sequence = 0u;
								bool base_is_formula = false;
								bool base_queued = false;
								if (generated_private_predecessor &&
									universal_dispatch_cache->generated_state_formula_ready)
								{
									base_queued =
										TryQueueGeneratedStateLoadFormulaContinuation(
											universal_dispatch_cache->generated_state_formula,
											addr == -1, &base_formula_sequence);
									base_is_formula = base_queued;
								}
								else if (universal_dispatch_cache->generated_product_ready)
								{
									base_queued =
										TryQueueGeneratedLoopKernelProductHotExecute(
											*universal_dispatch_cache, universal_entry_pc,
											static_cast<u16>(execution_vif_top),
											static_cast<u16>(execution_vif_itop),
											&m_deferred_vif_unpacks, &base_result,
											&hot_error);
								}
								if (base_queued)
								{
									u32 chain_executes = 1u;
									u32 chain_draws = base_is_formula ? 0u : 1u;
									u32 chain_formulas = base_is_formula ? 1u : 0u;
									u64 chain_pairs = base_is_formula ?
										universal_dispatch_cache->generated_state_formula.executed_pairs :
										base_result.executed_pairs;
									u64 first_sequence = base_is_formula ?
										base_formula_sequence : base_result.sequence;
									u64 last_sequence = first_sequence;
									s32 consumed_end = m_read_pos;
					for (BufferedGeneratedExecute& buffered :
											buffered_universal_executes)
									{
										if (!m_gpu_vu_generated_private_state.valid)
										{
											break;
										}
										const u32 following_entry_pc = buffered.addr == -1 ?
											m_gpu_vu_generated_private_state.tpc :
											((static_cast<u32>(buffered.addr) & 0x7ffu) << 3);
										UniversalDispatchCostCacheEntry* const following_cache =
											find_universal_dispatch_cache(following_entry_pc,
												buffered.vif_top, buffered.vif_itop,
												buffered.fbrst);
										if (!following_cache)
											break;

										bool following_queued = false;
										if (following_cache->generated_state_formula_ready)
										{
											u64 formula_sequence = 0u;
											std::swap(m_deferred_vif_unpacks,
												buffered.unpacks);
								following_queued =
									TryQueueGeneratedStateLoadFormulaContinuation(
										following_cache->generated_state_formula,
										buffered.addr == -1, &formula_sequence);
											if (!following_queued)
											{
												std::swap(m_deferred_vif_unpacks,
													buffered.unpacks);
											}
											else
											{
												following_cache->allow_multi_execute_gather = true;
												chain_formulas++;
												chain_pairs += following_cache->pair_count;
												last_sequence = formula_sequence;
											}
										}
										else if (following_cache->generated_product_ready)
										{
											GeneratedProductHotQueueResult following_result;
											following_queued =
												TryQueueGeneratedLoopKernelProductHotExecute(
													*following_cache, following_entry_pc,
													static_cast<u16>(buffered.vif_top),
													static_cast<u16>(buffered.vif_itop),
													&buffered.unpacks, &following_result,
													&hot_error);
											if (following_queued)
											{
												chain_draws++;
												chain_pairs += following_result.executed_pairs;
												last_sequence = following_result.sequence;
											}
										}
										if (!following_queued)
											break;

										chain_executes++;
										consumed_end = buffered.end_pos;
										vifRegs.top = buffered.vif_top;
										vifRegs.itop = buffered.vif_itop;
										vuFBRST = buffered.fbrst;
										if (VitaPerformanceTelemetry::IsEnabled() &&
											buffered.enqueued_at != 0u)
										{
											VitaGpuVu::RecordUniversalGpuVuMtvuExecuteQueueAge(
												MtvuElapsedTelemetryMicroseconds(
													buffered.enqueued_at));
										}
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
										Pcsx2Trace::NotifyMachineCheckpointVu1ProgramStarted();
#endif
									}
									if (consumed_end != m_read_pos)
										m_read_pos = consumed_end;

									m_gpu_vu_generated_unpublished_transactions += chain_draws;
									u32 published_draws = 0u;
									if (m_gpu_vu_generated_unpublished_transactions >=
										GeneratedLoopKernelAsyncPublicationExecutions)
									{
										const Common::Timer::Value publication_started =
											VitaPerformanceTelemetry::IsEnabled() ?
												Common::Timer::GetCurrentValue() : 0;
										published_draws =
											VitaGS::PublishMtvuPath1Completions();
										if (publication_started != 0)
										{
											VitaGpuVu::RecordGeneratedLoopKernelPublication(
												published_draws,
												MtvuElapsedTelemetryMicroseconds(
													publication_started));
										}
										if (published_draws != 0u)
										{
											m_gpu_vu_generated_unpublished_transactions = 0u;
											++m_gpu_vu_generated_async_publication_count;
										}
									}
									static u64 generated_chain_reports = 0u;
									static u64 generated_chain_prelude_us = 0u;
									static u64 generated_chain_dispatch_us = 0u;
									static u64 generated_chain_execute_total = 0u;
									const u64 chain_report = ++generated_chain_reports;
									if (generated_chain_dispatch_started != 0)
									{
										const Common::Timer::Value chain_finished =
											Common::Timer::GetCurrentValue();
										if (mtvu_record_started != 0 &&
											generated_chain_dispatch_started >= mtvu_record_started)
										{
											generated_chain_prelude_us += static_cast<u64>(
												Common::Timer::ConvertValueToSeconds(
													generated_chain_dispatch_started -
													mtvu_record_started) * 1000000.0);
										}
										generated_chain_dispatch_us += static_cast<u64>(
											Common::Timer::ConvertValueToSeconds(
												chain_finished - generated_chain_dispatch_started) *
											1000000.0);
									}
									generated_chain_execute_total += chain_executes;
									if (chain_report <= 8u ||
										(chain_report & (chain_report - 1u)) == 0u)
									{
										Console.WriteLn(
											"GPU-VU seq=%llu..%llu provider=generated-chain "
											"accepted=1 product_accepted=1 executes=%u pairs=%llu "
											"descriptor_objects=%u state_formulas=%u "
											"queue_publications=%u published_draws=%u "
											"output=direct-tfx cpu_vu_calls=0 "
											"cpu_semantic_pairs=0 full_state_copies=0 "
											"transaction_commit=pending "
											"cpu1_chain_prelude_us_per_call=%llu "
											"cpu1_chain_dispatch_us_per_call=%llu "
											"cpu1_chain_dispatch_us_per_execute=%llu.",
											static_cast<unsigned long long>(first_sequence),
											static_cast<unsigned long long>(last_sequence),
											chain_executes,
											static_cast<unsigned long long>(chain_pairs),
											chain_draws, chain_formulas,
											published_draws != 0u ? 1u : 0u,
											published_draws,
											static_cast<unsigned long long>(
												generated_chain_prelude_us / chain_report),
											static_cast<unsigned long long>(
												generated_chain_dispatch_us / chain_report),
											static_cast<unsigned long long>(
												generated_chain_execute_total != 0u ?
													generated_chain_dispatch_us /
														generated_chain_execute_total : 0u));
									}
									defer_vu_execution_completion = true;
									queued_direct_draw = true;
									break;
								}
							}
							if (additional_executes.empty() &&
								generated_private_predecessor &&
								universal_dispatch_cache &&
								universal_dispatch_cache->generated_state_formula_ready &&
							TryQueueGeneratedStateLoadFormulaContinuation(
								universal_dispatch_cache->generated_state_formula,
								addr == -1))
							{
								defer_vu_execution_completion = true;
								queued_direct_draw = true;
								break;
							}
							bool dispatch_policy_cache_hit = false;
							bool generated_product_hot_dispatch = false;
							VitaGpuVu::ShaderKey generated_product_hot_key{};
							if (additional_executes.empty() && universal_dispatch_cache &&
								universal_dispatch_cache->generated_product_ready)
							{
								const u64 cached_program_identity =
									universal_dispatch_cache->generated_program_identity;
								generated_product_hot_key = {
									universal_dispatch_cache->generated_key_low,
									universal_dispatch_cache->generated_key_high};
								generated_product_hot_dispatch =
									cached_program_identity != 0u &&
									(generated_product_hot_key.low != 0u ||
									 generated_product_hot_key.high != 0u);
								VitaGpuVu::RecordGeneratedLoopKernelProductHotDispatch(
									generated_product_hot_dispatch);
							if (generated_product_hot_dispatch)
							{
								generated_cold_program_identity = cached_program_identity;
								universal_pair_count =
									universal_dispatch_cache->pair_count;
								universal_dynamic_pair_bound =
									universal_dispatch_cache->dynamic_pair_upper_bound;
							}
							else
							{
								// A source/configuration generation or generated-registry
								// mismatch invalidates the shortcut before effects. The same
								// Execute immediately returns to canonical full preflight.
								universal_dispatch_cache->generated_product_ready = false;
								universal_dispatch_cache->generated_program_identity = 0;
							}
						}
						if (additional_executes.empty())
						{
							if (universal_dispatch_cache &&
								universal_dispatch_cache->skip_single_attempt &&
								!(generated_private_predecessor &&
								  universal_dispatch_cache->pair_count != 0u &&
								  universal_dispatch_cache->pair_count <= 32u &&
								  universal_dispatch_cache->dynamic_pair_upper_bound <= 32u))
							{
								const VitaGpuVu::ShaderKey generated_key{
									universal_dispatch_cache->generated_key_low,
									universal_dispatch_cache->generated_key_high};
									const VitaGpuVu::GeneratedProgramState generated_state =
										universal_dispatch_cache->wait_for_generated_program ?
											VitaGpuVu::QueryGeneratedProgram(generated_key) :
											VitaGpuVu::GeneratedProgramState::Missing;
									const VitaGpuVu::ShaderKey generated_attestation_key{
										universal_dispatch_cache->generated_attestation_key_low,
										universal_dispatch_cache->generated_attestation_key_high};
									const VitaGpuVu::GeneratedLoopKernelAttestationIdentity
										attestation_identity{
											generated_attestation_key,
											VitaGpuVu::GeneratedProgramAbiVersion,
											universal_dispatch_cache->generated_loop_kernel_abi,
											universal_dispatch_cache->configuration_bits,
											universal_dispatch_cache->generated_semantic_profile_key};
									const VitaGpuVu::GeneratedLoopKernelAttestationRecord
										attestation = universal_dispatch_cache->
											wait_for_generated_attestation ?
											VitaGpuVu::QueryGeneratedLoopKernelAttestation(
												attestation_identity) :
											VitaGpuVu::GeneratedLoopKernelAttestationRecord{};
								const u64 compiler_idle_generation =
									universal_dispatch_cache->
										wait_for_generated_compiler_idle ?
										VitaGpuVu::
											GetGeneratedProgramCompilerIdleGeneration() :
										universal_dispatch_cache->
											generated_compiler_idle_generation;
								const bool compiler_has_in_flight_work =
									universal_dispatch_cache->
										wait_for_generated_compiler_idle &&
									VitaGpuVu::GeneratedProgramCompilerHasInFlightWork();
									const bool compiler_idle_retry =
									universal_dispatch_cache->
										wait_for_generated_compiler_idle &&
										!compiler_has_in_flight_work &&
										universal_dispatch_cache->
											generated_compiler_idle_generation !=
											compiler_idle_generation;
									bool rejected_attestation_fallback_ready = false;
									VitaGpuVu::GeneratedLoopKernelBundle
										rejected_attestation_fallback;
									if (universal_dispatch_cache->wait_for_generated_attestation &&
										attestation.state == VitaGpuVu::
											GeneratedLoopKernelAttestationState::Rejected &&
										universal_dispatch_cache->generated_program_identity != 0u)
									{
										const auto fallback_state = VitaGpuVu::
											QueryGeneratedLoopKernelBundle(
												universal_dispatch_cache->generated_program_identity,
												universal_entry_pc, request.configuration_bits,
												&rejected_attestation_fallback);
										const VitaGpuVu::GeneratedLoopKernelAttestationIdentity
											fallback_identity{
												rejected_attestation_fallback.attestation_key,
												VitaGpuVu::GeneratedProgramAbiVersion,
												rejected_attestation_fallback.abi_version,
												rejected_attestation_fallback.configuration_bits,
												rejected_attestation_fallback.semantic_profile_key};
										const auto fallback_attestation = VitaGpuVu::
											QueryGeneratedLoopKernelAttestation(fallback_identity);
										rejected_attestation_fallback_ready =
											fallback_state == VitaGpuVu::
												GeneratedLoopKernelBundleState::Ready &&
											rejected_attestation_fallback.IsNoWriteProduct() &&
											rejected_attestation_fallback.MatchesOwner(
												universal_dispatch_cache->generated_program_identity,
												universal_entry_pc, request.configuration_bits,
												rejected_attestation_fallback.gif_tag) &&
											rejected_attestation_fallback.attestation_key !=
												attestation_identity.key &&
											fallback_attestation.state == VitaGpuVu::
												GeneratedLoopKernelAttestationState::Product;
										if (rejected_attestation_fallback_ready)
										{
											universal_dispatch_cache->generated_product_ready = true;
											universal_dispatch_cache->generated_key_low =
												rejected_attestation_fallback.kernel_key.low;
											universal_dispatch_cache->generated_key_high =
												rejected_attestation_fallback.kernel_key.high;
											universal_dispatch_cache->generated_attestation_key_low =
												rejected_attestation_fallback.attestation_key.low;
											universal_dispatch_cache->generated_attestation_key_high =
												rejected_attestation_fallback.attestation_key.high;
											universal_dispatch_cache->generated_loop_kernel_abi =
												rejected_attestation_fallback.abi_version;
											universal_dispatch_cache->generated_semantic_profile_key =
												rejected_attestation_fallback.semantic_profile_key;
											Console.WriteLn(
												"GPU-VU: rejected optional generated executable "
												"retired pre-effect; attested generated fallback "
												"resumed key=%016llx%016llx cpu_vu_calls=0.",
												static_cast<unsigned long long>(
													rejected_attestation_fallback.kernel_key.high),
												static_cast<unsigned long long>(
													rejected_attestation_fallback.kernel_key.low));
										}
									}
									const bool generated_ready =
										rejected_attestation_fallback_ready ||
										VitaGpuVu::GeneratedDispatchDependencyIsReady(
										universal_dispatch_cache->wait_for_generated_program,
										generated_state,
										universal_dispatch_cache->
											wait_for_generated_compiler_idle,
										universal_dispatch_cache->
											generated_compiler_idle_generation,
											compiler_idle_generation,
											compiler_has_in_flight_work) ||
										(universal_dispatch_cache->wait_for_generated_attestation &&
										 attestation.state != VitaGpuVu::
											GeneratedLoopKernelAttestationState::PrivateTest &&
										 attestation.state != VitaGpuVu::
											GeneratedLoopKernelAttestationState::Rejected);
									const bool generated_terminally_unavailable =
									universal_dispatch_cache->wait_for_generated_program &&
									VitaGpuVu::GeneratedProgramStateIsTerminallyUnavailable(
											generated_state) ||
										(!rejected_attestation_fallback_ready &&
										 universal_dispatch_cache->wait_for_generated_attestation &&
										 attestation.state == VitaGpuVu::
											GeneratedLoopKernelAttestationState::Rejected);
								if (generated_ready)
								{
									universal_dispatch_cache->skip_single_attempt = false;
									universal_dispatch_cache->wait_for_generated_program = false;
										universal_dispatch_cache->
											wait_for_generated_compiler_idle = false;
									universal_dispatch_cache->wait_for_generated_attestation =
										false;
									universal_dispatch_cache->generated_terminally_unavailable =
										false;
									if (compiler_idle_retry)
									{
										Console.WriteLn(
											"GPU-VU: structured generation retry released after "
											"compiler idle generation %llu -> %llu; "
											"pre-effect MTVU ownership remained authoritative.",
											static_cast<unsigned long long>(
												universal_dispatch_cache->
													generated_compiler_idle_generation),
											static_cast<unsigned long long>(
												compiler_idle_generation));
									}
								}
								else if (generated_terminally_unavailable)
								{
									// This exact generated root has reached a terminal compiler or
									// registration state.  Reopening full epoch preparation cannot
									// make that immutable source/configuration identity executable;
									// it only taxes MTVU before the same CPU fallback.  A later
									// microcode/configuration generation naturally gets a new cache
									// entry and may request another root.
									universal_dispatch_cache->skip_single_attempt = true;
									universal_dispatch_cache->wait_for_generated_program = false;
										universal_dispatch_cache->wait_for_generated_compiler_idle =
											false;
									universal_dispatch_cache->wait_for_generated_attestation =
										false;
									universal_dispatch_cache->generated_terminally_unavailable =
										true;
									universal_dispatch_cache->rejection = static_cast<u32>(
										VitaGpuVu::UniversalGpuVuRejection::
											GeneratedProgramUnavailable);
									universal_pair_count =
										universal_dispatch_cache->pair_count;
									universal_dynamic_pair_bound =
										universal_dispatch_cache->dynamic_pair_upper_bound;
									universal_rejection = VitaGpuVu::
										UniversalGpuVuRejection::GeneratedProgramUnavailable;
									dispatch_policy_cache_hit = true;
									VitaGpuVu::RecordUniversalGpuVuCachedPreflightRejection(
										universal_rejection);
								}
								else
								{
								universal_pair_count =
									universal_dispatch_cache->pair_count;
								universal_dynamic_pair_bound =
									universal_dispatch_cache->dynamic_pair_upper_bound;
								universal_rejection = static_cast<
									VitaGpuVu::UniversalGpuVuRejection>(
										universal_dispatch_cache->rejection);
								dispatch_policy_cache_hit = true;
								VitaGpuVu::RecordUniversalGpuVuCachedPreflightRejection(
									universal_rejection);
		}
	}
}
						if (!dispatch_policy_cache_hit)
						{
							if (!generated_product_hot_dispatch)
							{
								universal_epoch = VitaGpuVu::PrepareUniversalGpuVuEpoch(
									request, &universal_rejection, &universal_error,
									&universal_pair_count,
									&universal_dynamic_pair_bound);
								if (universal_epoch)
									generated_cold_program_identity =
										universal_epoch->ProgramIdentity();
								// Lookahead is only a batching optimization. Every base request is
								// already a complete E-bit transaction, so a later gathered Execute
								// exceeding a capacity or support boundary must not drag the supported
								// base epoch back to CPU. Retry the base immediately and leave every
								// looked-ahead queue record untouched for the next ordered attempt.
								if (!universal_epoch && !additional_executes.empty())
								{
									request.additional_executes = nullptr;
									additional_executes.clear();
									// These are shallow observations of queue-owned payload
									// references. The base-only retry leaves every lookahead
									// record queued, so discard the observations without
									// releasing their generation references during base cleanup.
									buffered_universal_executes.clear();
									buffered_universal_end_pos = m_read_pos;
									VitaGpuVu::
										RecordUniversalGpuVuMtvuMultiExecuteGatherSuppressed();
									universal_epoch = VitaGpuVu::PrepareUniversalGpuVuEpoch(
										request, &universal_rejection, &universal_error,
										&universal_pair_count,
										&universal_dynamic_pair_bound);
									if (universal_epoch)
										generated_cold_program_identity =
											universal_epoch->ProgramIdentity();
								}
							}
								VitaGpuVu::UniversalStateLoadFormula compiled_state_formula;
								if (universal_epoch && additional_executes.empty() &&
									generated_private_predecessor &&
								TryQueueGeneratedPrivateStateLoadContinuation(
									*universal_epoch, vuFBRST, addr == -1, false,
									&compiled_state_formula))
								{
									update_dispatch_cache(false, false,
										VitaGpuVu::UniversalGpuVuRejection::None,
										universal_pair_count,
										universal_dynamic_pair_bound);
									universal_dispatch_cache->generated_state_formula =
										compiled_state_formula;
									universal_dispatch_cache->generated_state_formula_ready = true;
								universal_epoch.reset();
								defer_vu_execution_completion = true;
								queued_direct_draw = true;
							}
							if (additional_executes.empty())
							{
								if (universal_epoch || generated_product_hot_dispatch)
								{
									const u64 generated_loop_program_identity =
										generated_product_hot_dispatch ?
											generated_cold_program_identity :
											universal_epoch->ProgramIdentity();
									const u32 generated_loop_analysis_entry_pc =
										generated_product_hot_dispatch ? universal_entry_pc :
											universal_epoch->AnalysisEntryPc();
									const u32 generated_loop_configuration_bits =
										request.configuration_bits;
									update_dispatch_cache(false, false,
										VitaGpuVu::UniversalGpuVuRejection::None,
										universal_pair_count,
										universal_dynamic_pair_bound);
									VitaGpuVu::GeneratedLoopKernelBundle
										generated_loop_kernel = generated_product_hot_dispatch ?
											VitaGpuVu::GeneratedLoopKernelBundle{} :
											universal_epoch->GeneratedLoopKernelDescriptor();
									VitaGpuVu::GeneratedLoopKernelLiveContract
										generated_loop_kernel_live_contract;
									bool generated_live_contract_resolved = false;
									if (generated_product_hot_dispatch ||
										generated_loop_kernel.HasAtomicCompilerOwnership())
									{
										const GeneratedLoopKernelPrivateState* const
											private_state = generated_private_predecessor ?
												&m_gpu_vu_generated_private_state : nullptr;
										static thread_local GpuVuMemoryView::BulkWorkspace memory_workspace;
										memory_workspace.Begin();
										GpuVuMemoryView memory_view{
											private_state ? reinterpret_cast<const u8*>(
												private_state->memory.data()) : VU1.Mem,
											&m_deferred_vif_unpacks,
											private_state ?
												private_state->unavailable_memory_words.data() :
												nullptr,
											&memory_workspace, VU1.Mem,
											private_state ?
												private_state->canonical_memory_qwords.data() :
												nullptr,
											private_state && private_state->raw_memory_provenance ?
												private_state->raw_memory_provenance.get() : nullptr};
										VitaGpuVu::InvocationEvaluationContext live_context;
										live_context.vif_top = request.vif_top;
										live_context.vif_itop = request.vif_itop;
										live_context.initial_vi = request.initial_vi.data();
										live_context.initial_vf_words = private_state ?
											&private_state->vf[0][0] : &VU1.VF[0].UL[0];
										live_context.initial_acc_words = private_state ?
											private_state->acc.data() : &VU1.ACC.UL[0];
										live_context.initial_q = private_state ?
											private_state->q : VU1.VI[REG_Q].UL;
										live_context.initial_p = private_state ?
											private_state->p : VU1.VI[REG_P].UL;
										live_context.initial_i = private_state ?
											private_state->i : VU1.VI[REG_I].UL;
										live_context.unavailable_initial_vf_lanes =
											private_state ?
												private_state->unavailable_vf_lanes.data() : nullptr;
										live_context.unavailable_initial_acc_lanes =
											private_state ? private_state->unavailable_acc_lanes : 0u;
										live_context.initial_q_available =
											!private_state || private_state->q_available;
										live_context.initial_p_available =
											!private_state || private_state->p_available;
										live_context.initial_i_available =
											!private_state || private_state->i_available;
										live_context.memory_user = &memory_view;
										live_context.read_memory_u16 = ReadGpuVuMemoryU16;
										live_context.read_memory_u32 = ReadGpuVuMemoryU32;
										live_context.read_memory_qwords = ReadGpuVuMemoryQwords;
										live_context.memory_qwords_have_canonical_owner =
											GpuVuMemoryQwordsHaveCanonicalOwner;
										live_context.bind_memory_qwords_to_raw_payload =
											BindGpuVuMemoryRawQwords;
										VitaGpuVu::GeneratedLoopKernelLiveContract resolved;
										std::string resolution_error;
										const Common::Timer::Value resolution_started =
											VitaPerformanceTelemetry::IsEnabled() ?
												Common::Timer::GetCurrentValue() : 0;
										if (generated_product_hot_dispatch)
										{
											generated_live_contract_resolved = VitaGpuVu::
												ResolveGeneratedLoopKernelLiveContract(
													generated_loop_program_identity,
													generated_loop_analysis_entry_pc,
													generated_loop_configuration_bits,
													generated_product_hot_key, live_context,
													&resolved, &resolution_error);
										}
										else
										{
											generated_live_contract_resolved = VitaGpuVu::
												ResolveGeneratedLoopKernelLiveContract(
													generated_loop_kernel, live_context, &resolved,
													&resolution_error);
										}
										if (resolution_started != 0)
										{
											VitaGpuVu::
												RecordGeneratedLoopKernelLiveContractResolution(
													MtvuElapsedTelemetryMicroseconds(
														resolution_started));
										}
										if (generated_live_contract_resolved)
										{
											generated_loop_kernel_live_contract =
												std::move(resolved);
											generated_loop_kernel =
												generated_loop_kernel_live_contract.descriptor;
										}
										else if (!resolution_error.empty())
										{
											universal_error = std::move(resolution_error);
										}
									}
										VitaGpuVu::UniversalStateLoadFormula
											compiled_no_output_formula;
										if (!generated_live_contract_resolved &&
											universal_epoch &&
											generated_private_predecessor &&
											VitaGpuVu::HasGeneratedNoPath1Observation(
												generated_loop_program_identity) &&
										TryQueueGeneratedPrivateStateLoadContinuation(
											*universal_epoch, vuFBRST, addr == -1, true,
											&compiled_no_output_formula))
										{
											update_dispatch_cache(false, false,
												VitaGpuVu::UniversalGpuVuRejection::None,
												universal_pair_count,
												universal_dynamic_pair_bound);
											if (compiled_no_output_formula.executed_pairs != 0u)
											{
												universal_dispatch_cache->generated_state_formula =
													compiled_no_output_formula;
												universal_dispatch_cache->generated_state_formula_ready =
													true;
											}
											universal_epoch.reset();
										defer_vu_execution_completion = true;
										queued_direct_draw = true;
										break;
									}
									const VitaGpuVu::ShaderKey generated_key =
										generated_loop_kernel.kernel_key;
									const bool has_generated_key =
										generated_key.low != 0 || generated_key.high != 0;
									const VitaGpuVu::GeneratedProgramState generated_state =
										has_generated_key ?
											VitaGpuVu::QueryGeneratedProgram(generated_key) :
											VitaGpuVu::GeneratedProgramState::Missing;
									const bool generated_loop_kernel_compiler_owned =
									generated_loop_kernel.MatchesOwner(
										generated_loop_program_identity,
										generated_loop_analysis_entry_pc,
										generated_loop_configuration_bits,
										generated_loop_kernel.gif_tag);
									const bool generated_loop_kernel_transaction_candidate =
										!generated_loop_kernel.
											requires_transactional_final_state &&
										generated_loop_kernel.uses_closed_form_nested_loop &&
										generated_loop_kernel.has_compact_final_state_formula &&
										generated_loop_kernel.private_store_count != 0u;
									const VitaGpuVu::GeneratedLoopKernelAttestationIdentity
										generated_loop_kernel_attestation_identity{
											generated_loop_kernel.attestation_key,
											VitaGpuVu::GeneratedProgramAbiVersion,
											generated_loop_kernel.abi_version,
											generated_loop_kernel.configuration_bits,
											generated_loop_kernel.semantic_profile_key};
									auto generated_loop_kernel_attestation =
										VitaGpuVu::QueryGeneratedLoopKernelAttestation(
											generated_loop_kernel_attestation_identity);
									if (generated_loop_kernel_attestation.state == VitaGpuVu::
										GeneratedLoopKernelAttestationState::Passed &&
										VitaGpuVu::PromoteGeneratedLoopKernelAttestationToProduct(
											generated_loop_kernel_attestation_identity))
									{
										generated_loop_kernel_attestation =
											VitaGpuVu::QueryGeneratedLoopKernelAttestation(
												generated_loop_kernel_attestation_identity);
										Console.WriteLn(
											"GPU-VU: generated loop-kernel attestation key=%016llx%016llx "
											"state=product profile=%s.",
											static_cast<unsigned long long>(
												generated_loop_kernel.kernel_key.high),
											static_cast<unsigned long long>(
												generated_loop_kernel.kernel_key.low),
											VitaGpuVu::GeneratedLoopKernelNumericProfileName(
												generated_loop_kernel_attestation.numeric_profile));
									}
									const bool generated_loop_kernel_candidate =
										generated_state ==
											VitaGpuVu::GeneratedProgramState::Ready &&
										generated_loop_kernel_compiler_owned &&
										generated_loop_kernel_transaction_candidate &&
										generated_loop_kernel_live_contract.HasPreEffectProof();
									const bool generated_loop_kernel_ready = VitaGpuVu::
										GeneratedLoopKernelProductExecutionIsReady(
											generated_state,
											generated_loop_kernel.IsNoWriteProduct(),
											generated_loop_kernel_compiler_owned,
											generated_loop_kernel_transaction_candidate,
											VitaGpuVu::
												GeneratedProgramHasProductResourceAttestation(
													generated_loop_kernel.kernel_key),
											generated_loop_kernel_attestation.state);
									const bool generated_loop_kernel_private_attestation =
										generated_loop_kernel_candidate &&
										generated_loop_kernel_attestation.state == VitaGpuVu::
											GeneratedLoopKernelAttestationState::Unattested;
									const bool generated_loop_kernel_attestation_pending =
										generated_loop_kernel_candidate &&
										generated_loop_kernel_attestation.state == VitaGpuVu::
											GeneratedLoopKernelAttestationState::PrivateTest;
									const bool generated_loop_kernel_attestation_rejected =
										generated_loop_kernel_candidate &&
										generated_loop_kernel_attestation.state == VitaGpuVu::
											GeneratedLoopKernelAttestationState::Rejected;
									bool generated_loop_kernel_preflight_failed = false;
									std::string generated_loop_kernel_error;
									if (generated_loop_kernel_attestation_rejected)
									{
										Console.WriteLn(
											"GPU-VU: generated loop-kernel key=%016llx%016llx "
											"attestation=rejected reason=%s pre_effect=1 "
											"cpu_fallback=1 pairs=%u.",
											static_cast<unsigned long long>(
												generated_loop_kernel.kernel_key.high),
											static_cast<unsigned long long>(
												generated_loop_kernel.kernel_key.low),
											VitaGpuVu::GeneratedLoopKernelAttestationRejectionName(
												generated_loop_kernel_attestation.rejection),
											universal_pair_count);
										universal_rejection = VitaGpuVu::
											UniversalGpuVuRejection::GeneratedProgramUnavailable;
										update_dispatch_cache(false, true, universal_rejection,
											universal_pair_count, universal_dynamic_pair_bound);
										universal_dispatch_cache->generated_terminally_unavailable =
											true;
										universal_epoch.reset();
									}
									else if (generated_loop_kernel_attestation_pending)
									{
										universal_dispatch_cache->skip_single_attempt = true;
										universal_dispatch_cache->wait_for_generated_attestation = true;
										universal_dispatch_cache->generated_key_low =
											generated_loop_kernel.kernel_key.low;
										universal_dispatch_cache->generated_key_high =
											generated_loop_kernel.kernel_key.high;
										universal_dispatch_cache->generated_attestation_key_low =
											generated_loop_kernel.attestation_key.low;
										universal_dispatch_cache->generated_attestation_key_high =
											generated_loop_kernel.attestation_key.high;
										universal_dispatch_cache->generated_loop_kernel_abi =
											generated_loop_kernel.abi_version;
										universal_dispatch_cache->generated_semantic_profile_key =
											generated_loop_kernel.semantic_profile_key;
										universal_dispatch_cache->generated_program_identity =
											generated_loop_program_identity;
										universal_rejection = VitaGpuVu::
											UniversalGpuVuRejection::GeneratedProgramPending;
										universal_dispatch_cache->rejection =
											static_cast<u32>(universal_rejection);
										universal_dispatch_cache->generated_terminally_unavailable =
											VitaGpuVu::
												GeneratedLoopKernelBundleStateIsTerminallyUnavailable(
													universal_epoch->GeneratedLoopKernelState());
										universal_epoch.reset();
									}
									else if (generated_loop_kernel_private_attestation)
									{
										bool comparison_prepared = false;
#if defined(__vita__)
											{
											const GeneratedLoopKernelPrivateState* const
												private_state = generated_private_predecessor ?
													&m_gpu_vu_generated_private_state : nullptr;
											static thread_local GpuVuMemoryView::BulkWorkspace memory_workspace;
											memory_workspace.Begin();
											GpuVuMemoryView memory_view{
												private_state ? reinterpret_cast<const u8*>(
													private_state->memory.data()) : VU1.Mem,
												&m_deferred_vif_unpacks,
												private_state ?
													private_state->unavailable_memory_words.data() :
													nullptr,
												&memory_workspace, VU1.Mem,
												private_state ?
													private_state->canonical_memory_qwords.data() :
													nullptr,
												private_state && private_state->raw_memory_provenance ?
													private_state->raw_memory_provenance.get() : nullptr};
											VitaGpuVu::InvocationEvaluationContext loop_context;
											loop_context.vif_top =
												static_cast<u16>(execution_vif_top);
											loop_context.vif_itop =
												static_cast<u16>(execution_vif_itop);
											loop_context.initial_vi = request.initial_vi.data();
											loop_context.initial_vf_words = private_state ?
												&private_state->vf[0][0] : &VU1.VF[0].UL[0];
											loop_context.initial_acc_words = private_state ?
												private_state->acc.data() : &VU1.ACC.UL[0];
											loop_context.initial_q = private_state ?
												private_state->q : VU1.VI[REG_Q].UL;
											loop_context.initial_p = private_state ?
												private_state->p : VU1.VI[REG_P].UL;
											loop_context.initial_i = private_state ?
												private_state->i : VU1.VI[REG_I].UL;
											loop_context.unavailable_initial_vf_lanes =
												private_state ?
													private_state->unavailable_vf_lanes.data() : nullptr;
											loop_context.unavailable_initial_acc_lanes =
												private_state ? private_state->unavailable_acc_lanes : 0u;
											loop_context.initial_q_available =
												!private_state || private_state->q_available;
											loop_context.initial_p_available =
												!private_state || private_state->p_available;
											loop_context.initial_i_available =
												!private_state || private_state->i_available;
											loop_context.memory_user = &memory_view;
											loop_context.read_memory_u16 = ReadGpuVuMemoryU16;
											loop_context.read_memory_u32 = ReadGpuVuMemoryU32;
											loop_context.read_memory_qwords = ReadGpuVuMemoryQwords;
											loop_context.memory_qwords_have_canonical_owner =
												GpuVuMemoryQwordsHaveCanonicalOwner;
											loop_context.bind_memory_qwords_to_raw_payload =
												BindGpuVuMemoryRawQwords;
											pending_loop_kernel_shadow_draw = VitaGpuVu::
												BuildGeneratedLoopKernelPrivateComparisonGpuVuDraw(
													generated_loop_kernel, loop_context,
													m_deferred_vif_unpacks,
													universal_dynamic_pair_bound,
													&generated_loop_kernel_error);
												if (pending_loop_kernel_shadow_draw)
												{
													pending_loop_kernel_shadow_validation_canary = true;
													pending_loop_kernel_shadow_pairs =
														pending_loop_kernel_shadow_draw->executed_pair_count;
													const u32 compact_input_qwords = static_cast<u32>(
														pending_loop_kernel_shadow_draw->CompactRawInputWords().size() /
														4u);
													comparison_prepared = true;
													Console.WriteLn(
														"GPU-VU: generated loop-kernel compiled=1 "
														"gpu_draw_pending=1 cpu_oracle=1 attestation=private-test "
														"pairs=%u input=%s "
														"input_qwords=%u output=direct-tfx "
														"product_accepted=0 validation_canary=1.",
														pending_loop_kernel_shadow_pairs,
														compact_input_qwords != 0u ? "compact" : "raw-ring",
														compact_input_qwords);
												}
											}
#endif
										universal_rejection = VitaGpuVu::
											UniversalGpuVuRejection::GeneratedProgramUnavailable;
										if (!comparison_prepared)
										{
											const u64 count =
												++generated_loop_kernel_preflight_fallback_count;
											if (count <= 8u || (count & (count - 1u)) == 0u)
											{
												Console.WriteLn(
													"GPU-VU: generated loop-kernel compiled=1 "
													"gpu_draw_pending=0 pre_effect=1 reason=%s "
														"fallback_cache=stable count=%llu; CPU MTVU remains "
													"authoritative.",
													generated_loop_kernel_error.empty() ?
														"transactional-final-state-unavailable" :
													generated_loop_kernel_error.c_str(),
												static_cast<unsigned long long>(count));
											}
											// Failure to construct this live descriptor is a
											// pre-effect rejection of this exact dispatch contract,
											// not physical evidence against the compiled GXP.  One
											// capacity root can serve several entry/count/input-owner
											// variants.  Marking a transient immutable-span failure as
											// InvalidDescriptor poisoned the shared executable before
											// BSpline's valid five-object contract could run its private
											// canary.  Only a queued private draw may publish terminal
											// attestation feedback from GS retirement.
											update_dispatch_cache(false, true,
												universal_rejection, universal_pair_count,
												universal_dynamic_pair_bound);
										}
										else
										{
											universal_dispatch_cache->skip_single_attempt = true;
											universal_dispatch_cache->wait_for_generated_attestation = true;
											universal_dispatch_cache->generated_key_low =
												generated_loop_kernel.kernel_key.low;
											universal_dispatch_cache->generated_key_high =
												generated_loop_kernel.kernel_key.high;
											universal_dispatch_cache->generated_attestation_key_low =
												generated_loop_kernel.attestation_key.low;
											universal_dispatch_cache->generated_attestation_key_high =
												generated_loop_kernel.attestation_key.high;
											universal_dispatch_cache->generated_loop_kernel_abi =
												generated_loop_kernel.abi_version;
											universal_dispatch_cache->generated_semantic_profile_key =
												generated_loop_kernel.semantic_profile_key;
											universal_dispatch_cache->generated_program_identity =
												generated_loop_program_identity;
											universal_dispatch_cache->rejection =
												static_cast<u32>(universal_rejection);
										}
										universal_epoch.reset();
									}
									else if (generated_loop_kernel_ready)
									{
										const GeneratedLoopKernelPrivateState* const
											private_state = generated_private_predecessor ?
												&m_gpu_vu_generated_private_state : nullptr;
									static thread_local GpuVuMemoryView::BulkWorkspace memory_workspace;
									memory_workspace.Begin();
									GpuVuMemoryView memory_view{
										private_state ? reinterpret_cast<const u8*>(
											private_state->memory.data()) : VU1.Mem,
										&m_deferred_vif_unpacks,
										private_state ?
											private_state->unavailable_memory_words.data() :
											nullptr,
										&memory_workspace, VU1.Mem,
										private_state ?
											private_state->canonical_memory_qwords.data() :
											nullptr,
										private_state && private_state->raw_memory_provenance ?
											private_state->raw_memory_provenance.get() : nullptr};
										VitaGpuVu::InvocationEvaluationContext loop_context;
										loop_context.vif_top =
											static_cast<u16>(execution_vif_top);
										loop_context.vif_itop =
											static_cast<u16>(execution_vif_itop);
										loop_context.initial_vi = request.initial_vi.data();
										loop_context.initial_vf_words = private_state ?
											&private_state->vf[0][0] : &VU1.VF[0].UL[0];
										loop_context.initial_acc_words = private_state ?
											private_state->acc.data() : &VU1.ACC.UL[0];
										loop_context.initial_q = private_state ?
											private_state->q : VU1.VI[REG_Q].UL;
										loop_context.initial_p = private_state ?
											private_state->p : VU1.VI[REG_P].UL;
									loop_context.initial_i = private_state ?
										private_state->i : VU1.VI[REG_I].UL;
									loop_context.unavailable_initial_vf_lanes =
										private_state ?
											private_state->unavailable_vf_lanes.data() : nullptr;
									loop_context.unavailable_initial_acc_lanes =
										private_state ? private_state->unavailable_acc_lanes : 0u;
									loop_context.initial_q_available =
										!private_state || private_state->q_available;
									loop_context.initial_p_available =
										!private_state || private_state->p_available;
									loop_context.initial_i_available =
										!private_state || private_state->i_available;
									loop_context.memory_user = &memory_view;
									loop_context.read_memory_u16 = ReadGpuVuMemoryU16;
									loop_context.read_memory_u32 = ReadGpuVuMemoryU32;
									loop_context.read_memory_qwords = ReadGpuVuMemoryQwords;
									loop_context.memory_qwords_have_canonical_owner =
										GpuVuMemoryQwordsHaveCanonicalOwner;
									loop_context.bind_memory_qwords_to_raw_payload =
										BindGpuVuMemoryRawQwords;
										const Common::Timer::Value descriptor_started =
											VitaPerformanceTelemetry::IsEnabled() ?
												Common::Timer::GetCurrentValue() : 0;
										auto loop_draw = VitaGpuVu::
											BuildGeneratedLoopKernelGpuVuDraw(
												std::move(generated_loop_kernel_live_contract),
												loop_context,
												m_deferred_vif_unpacks,
												universal_dynamic_pair_bound,
												generated_loop_kernel_attestation.numeric_profile,
												&generated_loop_kernel_error);
										if (descriptor_started != 0)
										{
											VitaGpuVu::RecordGeneratedLoopKernelDescriptorBuild(
												MtvuElapsedTelemetryMicroseconds(
													descriptor_started));
										}
										if (loop_draw)
										{
											const u32 vertices = loop_draw->vertex_count;
											const u32 primitives = loop_draw->primitive_count;
											const u32 executed_pairs =
												loop_draw->executed_pair_count;
											const u32 resume_pc = loop_draw->unique_resume_pc;
									const auto generated_transaction =
										loop_draw->GeneratedLoopKernelTransactionOwner();
									GeneratedLoopKernelPrivateAdvanceProof
										private_advance_proof;
										const bool private_successor_available =
											generated_transaction &&
											CanAdvanceGeneratedLoopKernelPrivateState(
												*generated_transaction,
												m_deferred_vif_unpacks,
												&private_advance_proof);
											bool exact_successor_available =
												private_successor_available;
												const u64 generated_sequence =
													VitaGpuVu::NextGpuVuOrderingSequence();
												const bool descriptor_sealed =
													exact_successor_available &&
													loop_draw->SealGeneratedLoopKernelProductForQueue(
													generated_sequence,
													&generated_loop_kernel_error);
										const bool journal_attached = descriptor_sealed &&
											generated_transaction &&
											generated_transaction->AttachReplayUnpacks(
												&m_deferred_vif_unpacks);
										const bool journal_proof_bound = journal_attached &&
											private_advance_proof.Matches(
												*generated_transaction,
												generated_transaction->ReplayUnpacks());
										if (journal_attached && !journal_proof_bound)
										{
											const bool recovered = generated_transaction->
												RecoverReplayUnpacksBeforeGsAcceptance(
													&m_deferred_vif_unpacks);
											pxAssertRel(recovered,
												"generated transaction could not recover a moved proof journal");
										}
										if (descriptor_sealed && !journal_proof_bound &&
											generated_transaction)
										{
											generated_transaction->MarkFailed(
												VitaGpuVu::GeneratedLoopKernelTransactionFailure::
													CpuJournalAttachment,
												journal_attached ?
													"replay journal storage changed while binding pre-effect proof" :
													"attach replay journal before generated mailbox publication");
										}
										const bool descriptor_queued = journal_proof_bound &&
											VitaGS::QueueGpuVuDraw(std::move(loop_draw));
										if (descriptor_queued)
										{
											// Queue acceptance is the final product proof for this exact
											// worker-owned source generation and configuration. Subsequent
											// Executes may bypass universal epoch allocation/encoding, but
											// still resolve the live contract transactionally above.
											universal_dispatch_cache->allow_multi_execute_gather = true;
											universal_dispatch_cache->generated_product_ready = true;
											m_gpu_vu_generated_generation_had_product = true;
											universal_dispatch_cache->generated_program_identity =
												generated_loop_program_identity;
											universal_dispatch_cache->generated_key_low =
												generated_loop_kernel.kernel_key.low;
											universal_dispatch_cache->generated_key_high =
												generated_loop_kernel.kernel_key.high;
											universal_dispatch_cache->generated_attestation_key_low =
												generated_loop_kernel.attestation_key.low;
											universal_dispatch_cache->generated_attestation_key_high =
												generated_loop_kernel.attestation_key.high;
											universal_dispatch_cache->generated_loop_kernel_abi =
												generated_loop_kernel.abi_version;
											universal_dispatch_cache->generated_semantic_profile_key =
												generated_loop_kernel.semantic_profile_key;
											{
												const Common::Timer::Value advance_started =
												VitaPerformanceTelemetry::IsEnabled() ?
													Common::Timer::GetCurrentValue() : 0;
									AdvanceGeneratedLoopKernelPrivateState(
										*generated_transaction,
										generated_transaction->ReplayUnpacks(),
										private_advance_proof,
										nullptr);
									if (generated_transaction->HasDeferredSuccessor())
									{
										pxAssertRel(generated_transaction->
											ReleaseTransferredDeferredSuccessor(),
											"generated product retained a transferred exact successor graph");
									}
									if (generated_transaction->
									UsesDeferredPairPlanStoreCommit())
								{
									pxAssertRel(generated_transaction->
										ReleaseTransferredDeferredStores(),
										"generated product retained a transferred exact store graph");
								}
											if (advance_started != 0)
											{
												VitaGpuVu::
													RecordGeneratedLoopKernelPrivateStateAdvance(
														MtvuElapsedTelemetryMicroseconds(
															advance_started));
											}
											}
												m_deferred_vif_unpack_count.store(
													0u, std::memory_order_release);
										m_gpu_vu_generated_pending_transactions.push_back(
											generated_transaction);
										m_gpu_vu_generated_private_tail_sequence =
											generated_sequence;
										PublishGeneratedLoopKernelPendingExecutionCount();
										// Do not phase-serialize CPU1 descriptor construction and
										// CPU2 GXM command encoding.  This is the non-visibility
										// publication seam: it preserves one ordinary scene/job and
										// lets EndScene retire the ordered run asynchronously.
										m_gpu_vu_generated_unpublished_transactions++;
										if (m_gpu_vu_generated_unpublished_transactions >=
											GeneratedLoopKernelAsyncPublicationExecutions)
										{
											const u32 published_draws =
												VitaGS::PublishMtvuPath1Completions();
											if (published_draws != 0u)
											{
												m_gpu_vu_generated_unpublished_transactions = 0;
												const u64 publication =
													++m_gpu_vu_generated_async_publication_count;
												if (publication <= 8u ||
													(publication & (publication - 1u)) == 0u)
												{
													Console.WriteLn(
														"GPU-VU generated_batch_publish=async "
														"publication=%llu published_draws=%u "
														"submit_tail_retained=1 visibility=deferred "
														"firmware_jobs=0 pending=%u.",
														static_cast<unsigned long long>(publication),
														published_draws,
														GeneratedLoopKernelPendingExecutionCount());
												}
											}
										}
										// Queue acceptance publishes the exact speculative successor
										// to CPU1. Physical vertex completion gates canonical state
										// and resource retirement, not ordinary EE progress.
										m_execute_jobs_completed.fetch_add(
											1u, std::memory_order_release);
										m_ring_space_progress.NotifyOfProgress();
								VitaGpuVu::RecordUniversalGpuVuAsyncQueued(
									GeneratedLoopKernelPendingExecutionCount());
										defer_vu_execution_completion = true;
										queued_direct_draw = true;
											if (generated_sequence <= 8u ||
												(generated_sequence & (generated_sequence - 1u)) == 0u)
											{
												Console.WriteLn(
													"GPU-VU seq=%llu provider=generated-loop-kernel "
													"accepted=1 product_accepted=1 pending_commit=1 pairs=%u "
													"pre_loop_stores=%u vertices=%u primitives=%u "
											"roots=1 draws=1 firmware_jobs=pending-batch "
											"batch_depth=%u private_successor=%u "
											"output=direct-tfx cpu_vu_calls=0 resume_pc=%04x "
													"validation_canary=%u.",
													static_cast<unsigned long long>(
														generated_sequence),
													executed_pairs,
													generated_transaction ?
														generated_transaction->PreLoopStoreEntryCount() : 0u,
											vertices, primitives,
											GeneratedLoopKernelPendingExecutionCount(),
											private_successor_available ? 1u : 0u,
											resume_pc,
													generated_transaction &&
														generated_transaction->IsValidationCanary() ? 1u : 0u);
											}
											}
												else
												{
													bool recovered = true;
													if (journal_proof_bound && generated_transaction)
													{
														recovered = generated_transaction->
															RecoverReplayUnpacksBeforeGsAcceptance(
																&m_deferred_vif_unpacks);
														pxAssertRel(recovered,
															"rejected generated GPU-VU draw lost its replay journal");
													}
													if (generated_loop_kernel_error.empty())
													{
														generated_loop_kernel_error = !descriptor_sealed ?
															"generated descriptor sealing failed" :
															!journal_attached ?
																"generated transaction replay journal attachment failed" :
															!journal_proof_bound ?
																"generated transaction replay journal changed proof storage" :
															recovered ?
																"direct draw mailbox rejected the descriptor" :
																"direct draw mailbox rejected after GXM acceptance";
													}
													generated_loop_kernel_preflight_failed = true;
											}
										}
										else
										{
											generated_loop_kernel_preflight_failed = true;
										}
									}
									if (generated_loop_kernel_preflight_failed)
									{
										if (universal_error.empty())
											universal_error = generated_loop_kernel_error;
										const u64 count =
											++generated_loop_kernel_preflight_fallback_count;
										if (count <= 8u || (count & (count - 1u)) == 0u)
										{
											Console.WriteLn(
												"GPU-VU: generated loop-kernel pre-effect "
												"fallback count=%llu reason=%s; no GPU effect "
												"was published and CPU MTVU remains authoritative.",
												static_cast<unsigned long long>(count),
												generated_loop_kernel_error.empty() ?
													"descriptor construction failed" :
													generated_loop_kernel_error.c_str());
										}
										universal_rejection = VitaGpuVu::
											UniversalGpuVuRejection::GeneratedProgramUnavailable;
										universal_epoch.reset();
									}
									if (universal_epoch && !queued_direct_draw)
									{
										const bool generated_pending =
											VitaGpuVu::GeneratedProgramStateIsPending(
												generated_state);
										const bool compiler_retry =
											universal_epoch->GeneratedRequestDeferred();
										const bool output_contract_retry =
											universal_epoch->
												GeneratedLoopKernelOutputContractPending();
									const bool terminal_bundle_variant =
										!output_contract_retry &&
										VitaGpuVu::
											GeneratedLoopKernelBundleStateIsTerminallyUnavailable(
												universal_epoch->GeneratedLoopKernelState());
									// A runtime GIF/output variant is not part of the immutable VU
									// source identity. If this generation has already queued an
									// attested product, keep single-entry resolution open so a GS
									// debug-mode round trip can return to that cached product.
									const bool retain_runtime_variant_retry =
										terminal_bundle_variant &&
										m_gpu_vu_generated_generation_had_product;
									universal_dispatch_cache->skip_single_attempt =
										!output_contract_retry && !retain_runtime_variant_retry;
									if (!generated_pending && !compiler_retry &&
										universal_dispatch_cache->skip_single_attempt)
									{
										// Preserve the original gate before the stable dispatch
										// cache discards this epoch and its resolution error.
										// Repeated cache hits intentionally do no new proof work.
										const u64 report = ++generated_loop_kernel_dispatch_rejection_count;
										if (report <= 16u || (report & (report - 1u)) == 0u)
										{
											Console.WriteLn(
												"GPU-VU: generated dispatch gate rejected report=%llu "
												"identity=%016llx entry=%04x key=%016llx%016llx "
												"bundle_state=%u program_state=%u source_abi=%u "
												"owner=%u transaction=%u resolved=%u proof=%u "
												"attestation=%u counts=%u:%u candidates=%u "
												"tag=%08x:%08x:%08x:%08x reason=%s "
												"pre_effect=1 fallback_cache=stable.",
												static_cast<unsigned long long>(report),
												static_cast<unsigned long long>(generated_loop_program_identity),
												generated_loop_analysis_entry_pc,
												static_cast<unsigned long long>(generated_key.high),
												static_cast<unsigned long long>(generated_key.low),
												static_cast<u32>(universal_epoch->GeneratedLoopKernelState()),
												static_cast<u32>(generated_state),
												generated_loop_kernel.loop_kernel_source_abi,
												static_cast<u32>(generated_loop_kernel_compiler_owned),
												static_cast<u32>(generated_loop_kernel_transaction_candidate),
												static_cast<u32>(generated_live_contract_resolved),
												static_cast<u32>(generated_loop_kernel_live_contract.HasPreEffectProof()),
												static_cast<u32>(generated_loop_kernel_attestation.state),
												generated_loop_kernel_live_contract.entry_iterations,
												generated_loop_kernel_live_contract.outer_iterations,
												generated_loop_kernel_live_contract.runtime_candidate_count,
												generated_loop_kernel.gif_tag[0], generated_loop_kernel.gif_tag[1],
												generated_loop_kernel.gif_tag[2], generated_loop_kernel.gif_tag[3],
												universal_error.empty() ? "none" : universal_error.c_str());
										}
									}
										universal_dispatch_cache->wait_for_generated_program =
											!output_contract_retry && generated_pending;
										universal_dispatch_cache->wait_for_generated_compiler_idle =
											!output_contract_retry && compiler_retry;
										universal_dispatch_cache->generated_key_low =
											generated_key.low;
										universal_dispatch_cache->generated_key_high =
											generated_key.high;
										universal_dispatch_cache->
											generated_compiler_idle_generation =
												universal_epoch->
													GeneratedRequestObservedCompilerIdle();
										universal_rejection =
											generated_pending || compiler_retry ?
												VitaGpuVu::UniversalGpuVuRejection::
													GeneratedProgramPending :
												VitaGpuVu::UniversalGpuVuRejection::
													GeneratedProgramUnavailable;
										universal_dispatch_cache->rejection =
											static_cast<u32>(universal_rejection);
									universal_dispatch_cache->generated_terminally_unavailable =
										terminal_bundle_variant &&
										!retain_runtime_variant_retry;
										universal_epoch.reset();
									}
								}
								else if (IsUniversalGpuVuEntryStableRejection(
									universal_rejection))
								{
									update_dispatch_cache(false, true,
										universal_rejection, universal_pair_count,
										universal_dynamic_pair_bound);
								}
							}
							else if (!universal_epoch && universal_dispatch_cache)
							{
								// The base entry remains eligible by itself, but this exact
								// queue shape did not form a complete supported transaction.
								// Return to one-entry classification instead of repeatedly
								// walking the same sliding lookahead window.
								universal_dispatch_cache->allow_multi_execute_gather = false;
							}
							}
							if (queued_direct_draw)
								break;
						if (!universal_epoch)
					{
							if (report_universal_fallback())
								{
									const u64 fallback_identity =
										generated_cold_program_identity != 0u ?
											generated_cold_program_identity :
										universal_dispatch_cache ?
											universal_dispatch_cache->generated_program_identity : 0u;
									const auto& physical_transactions =
										m_gpu_vu_generated_pending_transactions;
									const auto* front_transaction =
										physical_transactions.empty() ? nullptr :
											physical_transactions.front().get();
									const auto* back_transaction =
										physical_transactions.empty() ? nullptr :
											physical_transactions.back().get();
									Console.WriteLn(
										"GPU-VU seq=0 provider=cpu-mtvu accepted=0 "
										"pre_effect=1 reason=%s identity=%016llx entry=%04x "
										"pairs=%u dynamic_bound=%u private_generation=%u "
										"pending_transactions=%u physical_pending=%u "
										"front=%llu:%s back=%llu:%s private_tail=%llu "
										"private_tpc=%04x continuations=%u unpublished=%u "
										"cache=%u:%u:%u cache_identity=%016llx "
										"cache_key=%016llx:%016llx cache_abi=%u "
										"cache_attestation=%016llx:%016llx "
										"cache_micro_generation=%llu cache_input=%04x:%04x:%03x "
										"cache_flags=%u:%u:%u:%u detail=%s "
										"watchdog_budget=%u cpu_fallback=1 "
										"fallback_count=%llu",
									VitaGpuVu::UniversalGpuVuRejectionName(
										universal_rejection),
									static_cast<unsigned long long>(fallback_identity),
									universal_entry_pc,
									universal_pair_count,
									universal_dynamic_pair_bound,
										m_gpu_vu_generated_private_state.valid ? 1u : 0u,
										GeneratedLoopKernelPendingExecutionCount(),
										static_cast<u32>(physical_transactions.size()),
										static_cast<unsigned long long>(front_transaction ?
											front_transaction->Sequence() : 0u),
										front_transaction ?
											VitaGpuVu::GeneratedLoopKernelTransactionStageName(
												front_transaction->Stage()) : "none",
										static_cast<unsigned long long>(back_transaction ?
											back_transaction->Sequence() : 0u),
										back_transaction ?
											VitaGpuVu::GeneratedLoopKernelTransactionStageName(
												back_transaction->Stage()) : "none",
										static_cast<unsigned long long>(
											m_gpu_vu_generated_private_tail_sequence),
										m_gpu_vu_generated_private_state.tpc,
										static_cast<u32>(
											m_gpu_vu_generated_private_continuations.size()),
										m_gpu_vu_generated_unpublished_transactions,
										universal_dispatch_cache ? 1u : 0u,
									universal_dispatch_cache &&
										universal_dispatch_cache->generated_product_ready ? 1u : 0u,
										universal_dispatch_cache &&
											universal_dispatch_cache->generated_state_formula_ready ? 1u : 0u,
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->generated_program_identity : 0u),
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->generated_key_low : 0u),
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->generated_key_high : 0u),
										universal_dispatch_cache ?
											universal_dispatch_cache->generated_loop_kernel_abi : 0u,
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->generated_attestation_key_low : 0u),
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->generated_attestation_key_high : 0u),
										static_cast<unsigned long long>(universal_dispatch_cache ?
											universal_dispatch_cache->micro_generation : 0u),
										universal_dispatch_cache ?
											universal_dispatch_cache->vif_top : 0u,
										universal_dispatch_cache ?
											universal_dispatch_cache->vif_itop : 0u,
										universal_dispatch_cache ?
											universal_dispatch_cache->observer_fbrst : 0u,
										universal_dispatch_cache &&
											universal_dispatch_cache->skip_single_attempt ? 1u : 0u,
										universal_dispatch_cache &&
											universal_dispatch_cache->wait_for_generated_program ? 1u : 0u,
										universal_dispatch_cache &&
											universal_dispatch_cache->wait_for_generated_attestation ? 1u : 0u,
										universal_dispatch_cache &&
											universal_dispatch_cache->generated_terminally_unavailable ? 1u : 0u,
										universal_error.empty() ? "none" : universal_error.c_str(),
									VitaGpuVu::
										UniversalGpuVuWatchdogSafePairsPerSubmission,
									static_cast<unsigned long long>(
										universal_fallback_count));
							}
							VitaGpuVu::RecordUniversalGpuVuPreflightCpuFallback(
								universal_pair_count);
						}
								else
						{
							// The hardware-driven product has exactly one GPU submission
							// seam above: QueueGpuVuDraw() for an attested loop-kernel.
							// Reaching this point with an epoch would otherwise revive the
							// rejected fixed/serial or multi-job owner.
							const u64 rejected_sequence = universal_epoch->Sequence();
							universal_rejection = VitaGpuVu::
								UniversalGpuVuRejection::GeneratedArchitectureQuarantined;
							if (report_universal_fallback())
							{
								Console.WriteLn(
									"GPU-VU seq=%llu provider=cpu-mtvu accepted=0 "
									"pre_effect=1 reason=%s pairs=%u dynamic_bound=%u "
									"legacy_gpu_jobs=0 cpu_fallback=1 fallback_count=%llu",
									static_cast<unsigned long long>(rejected_sequence),
									VitaGpuVu::UniversalGpuVuRejectionName(
										universal_rejection),
									universal_pair_count, universal_dynamic_pair_bound,
									static_cast<unsigned long long>(
										universal_fallback_count));
							}
							VitaGpuVu::RecordUniversalGpuVuPreflightCpuFallback(
								universal_pair_count);
							universal_epoch.reset();
						}
					}
#endif

						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"CPU VU fallback could not drain universal predecessors");
						const Common::Timer::Value materialize_start =
						VitaPerformanceTelemetry::IsEnabled() ?
							Common::Timer::GetCurrentValue() : 0;
					if (!MaterializeUniversalGpuVuStateForCpu())
					{
						Console.Error(
							"GPU-VU: CPU fallback cannot proceed from stale VU1 state.");
						break;
					}
					if (materialize_start != 0)
					{
						VitaGpuVu::RecordUniversalGpuVuCpuMaterializeTime(
							MtvuElapsedTelemetryMicroseconds(materialize_start));
					}
					// Materializing the preceding GPU generation restores its terminal
					// TPC. A new MSCAL owns an explicit entry and must replace that TPC;
					// MSCNT alone resumes the committed terminal PC. The earlier
					// SetStartPC() precedes the ownership transfer and is therefore not
					// sufficient when fallback follows an accepted universal epoch.
					if (addr != -1)
						VU1.VI[REG_TPC].UL = static_cast<u32>(addr) & 0x7ffu;
					CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
					VU1.cycle = 0;
					const Common::Timer::Value unpack_replay_start =
						VitaPerformanceTelemetry::IsEnabled() ?
							Common::Timer::GetCurrentValue() : 0;
					ReplayDeferredVifUnpacks();
					if (unpack_replay_start != 0)
					{
						VitaGpuVu::RecordUniversalGpuVuCpuUnpackReplayTime(
							MtvuElapsedTelemetryMicroseconds(unpack_replay_start));
					}
					// PCSX2 Vif_Codes.cpp::vuExecMicro() publishes TOP/ITOP
					// for this exact MSCAL/MSCNT before it advances the VIF1
					// double buffer. Replaying an earlier UNPACK reconstructs
					// that command's VIF registers as an implementation detail;
					// it must not replace the execution snapshot observed by
					// XTOP/XITOP in the VU program.
					vifRegs.top = execution_vif_top;
					vifRegs.itop = execution_vif_itop;
					std::array<u16, 16> generated_cold_initial_vi{};
					for (u32 reg = 0; reg < generated_cold_initial_vi.size(); reg++)
						generated_cold_initial_vi[reg] = VU1.VI[reg].US[0];
					BeginProgram();
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					const bool gpu_vu_census_enabled =
						VitaGpuVuOpportunityCensus::IsEnabled();
					const u64 gpu_vu_census_cpu_start = gpu_vu_census_enabled ?
						Threading::GetThreadCpuTime() : 0;
					const u64 gpu_vu_census_cycle_start = VU1.cycle;
					if (gpu_vu_census_enabled)
					{
						VitaGpuVuOpportunityCensus::BeginVuExecute(
							VU1.Micro, VU1_PROGSIZE,
							VU1.VI[REG_TPC].UL << 3, addr == -1, vuFBRST);
					}
#endif
					const Common::Timer::Value cpu_fallback_start =
						VitaPerformanceTelemetry::IsEnabled() ?
							Common::Timer::GetCurrentValue() : 0;
					CpuVU1->Execute(vu1RunCycles);
						if (cpu_fallback_start != 0)
						{
							VitaGpuVu::RecordUniversalGpuVuCpuFallbackTime(
								MtvuElapsedTelemetryMicroseconds(cpu_fallback_start));
						}
					// gsPack accumulates this dispatch's XGKICK bytes and is reset
					// by FinishGSPacketMTVU, so sample it first.
					const u32 cpu_path1_bytes =
						gifUnit.gifPath[GIF_PATH_1].gsPack.size;
					const Gif_Path& generated_cold_path1 =
						gifUnit.gifPath[GIF_PATH_1];
					const bool generated_cold_path1_valid =
						generated_cold_path1.buffer &&
						generated_cold_path1.gsPack.offset <=
							generated_cold_path1.buffSize &&
						cpu_path1_bytes <=
							generated_cold_path1.buffSize -
								generated_cold_path1.gsPack.offset;
					if (generated_cold_program_identity != 0 &&
						cpu_path1_bytes >= sizeof(std::array<u32, 4>) &&
						generated_cold_path1_valid)
					{
						std::array<u32, 4> generated_cold_tag{};
						std::memcpy(generated_cold_tag.data(),
							generated_cold_path1.buffer +
								generated_cold_path1.gsPack.offset,
							sizeof(generated_cold_tag));
						VitaGpuVu::RecordGeneratedNestedDirectPath1Tag(
							generated_cold_program_identity, generated_cold_tag,
							&generated_cold_initial_vi,
							static_cast<u16>(execution_vif_top),
							static_cast<u16>(execution_vif_itop));
					}
					else if (generated_cold_program_identity != 0)
					{
						VitaGpuVu::RecordGeneratedNestedDirectNoPath1(
							generated_cold_program_identity);
					}
					#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
					if (universal_epoch && universal_rejection ==
							VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1)
					{
						const u32 address = universal_epoch->RejectedPath1Address();
						std::array<u32, 4> cpu_tag{};
						std::memcpy(cpu_tag.data(), VU1.Mem + address,
							cpu_tag.size() * sizeof(u32));
						const auto& gpu_tag = universal_epoch->RejectedPath1Tag();
						Console.Warning(
							"GPU-VU seq=%llu provider=universal replay-path1-oracle "
							"address=%04x gpu_tag=%08x:%08x:%08x:%08x "
							"cpu_tag=%08x:%08x:%08x:%08x cpu_path1_bytes=%u",
							static_cast<unsigned long long>(universal_epoch->Sequence()),
							address, gpu_tag[0], gpu_tag[1], gpu_tag[2], gpu_tag[3],
							cpu_tag[0], cpu_tag[1], cpu_tag[2], cpu_tag[3],
							cpu_path1_bytes);
					}
					#endif
					VitaGpuVu::RecordCpuVu1Execution(
						cpu_path1_bytes);
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					if (gpu_vu_census_enabled)
					{
						VitaGpuVuOpportunityCensus::EndVuExecute(
							Threading::GetThreadCpuTime() -
								gpu_vu_census_cpu_start,
							VU1.cycle - gpu_vu_census_cycle_start,
							cpu_path1_bytes, m_program_active);
					}
#endif
						#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						bool loop_kernel_shadow_queued = false;
						if (pending_loop_kernel_shadow_draw)
						{
							const u32 vertices =
								pending_loop_kernel_shadow_draw->vertex_count;
							const u32 primitives =
								pending_loop_kernel_shadow_draw->primitive_count;
							pending_loop_kernel_shadow_draw->ordering_sequence =
								VitaGpuVu::NextGpuVuOrderingSequence();
							const u64 sequence =
								pending_loop_kernel_shadow_draw->ordering_sequence;
							if (pending_loop_kernel_shadow_draw->private_replay_capture_id != 0u)
								Console.WriteLn("GPU-VU seq=%llu private_replay_capture=%llu pre_queue=1.",
									static_cast<unsigned long long>(sequence),
									static_cast<unsigned long long>(pending_loop_kernel_shadow_draw->private_replay_capture_id));
							const std::array<u32, 4> gpu_shadow_tag =
								pending_loop_kernel_shadow_draw->gif_tag;
							std::array<u32, 4> cpu_shadow_tag{};
							const bool cpu_shadow_tag_available =
								cpu_path1_bytes >= sizeof(cpu_shadow_tag) &&
								generated_cold_path1_valid;
							if (cpu_shadow_tag_available)
							{
								std::memcpy(cpu_shadow_tag.data(),
									generated_cold_path1.buffer +
										generated_cold_path1.gsPack.offset,
									sizeof(cpu_shadow_tag));
							}
								const bool cpu_shadow_tag_matches =
									cpu_shadow_tag_available &&
									cpu_shadow_tag == gpu_shadow_tag;
								const auto attestation_identity =
									pending_loop_kernel_shadow_draw->
										GeneratedLoopKernelAttestation();
								u32 architectural_state_mismatches = 0u;
								u32 architectural_state_playable_mismatches = 0u;
								std::array<u16, 16> architectural_state_final_vi{};
								for (u32 reg = 0u;
									reg < architectural_state_final_vi.size(); reg++)
								{
									architectural_state_final_vi[reg] =
										VU1.VI[reg].US[0];
								}
								VitaGpuVu::PrivateArchitecturalStateComparison
									architectural_state_comparison;
								const bool architectural_state_compared =
									pending_loop_kernel_shadow_draw->
										ComparePrivateArchitecturalState(
										&VU1.VF[0].UL[0], &VU1.ACC.UL[0],
										VU1.VI[REG_Q].UL, VU1.VI[REG_P].UL,
										VU1.VI[REG_I].UL,
										architectural_state_final_vi,
										VU1.VI[REG_TPC].UL << 3,
											&architectural_state_mismatches,
											&architectural_state_playable_mismatches,
											&architectural_state_comparison);
								if (architectural_state_compared &&
									architectural_state_mismatches != 0u)
								{
									for (u32 mismatch = 0u;
										mismatch < architectural_state_comparison.
											exact_mismatch_count;
										mismatch++)
									{
										const auto& detail = architectural_state_comparison.
											exact_mismatches[mismatch];
										Console.Warning(
											"GPU-VU seq=%llu provider=generated-loop-kernel "
											"architectural_state_formula=MISMATCH "
											"detail=%u/%u kind=%s reg=%u lane=%u "
											"cpu=%08x exact_formula=%08x "
											"playable_formula=%08x product_accepted=0.",
											static_cast<unsigned long long>(sequence),
											mismatch + 1u,
											architectural_state_comparison.
												exact_mismatch_total,
											VitaGpuVu::PrivateArchitecturalStateValueKindName(
												detail.kind),
											detail.reg, detail.lane, detail.actual,
											detail.exact_expected,
											detail.playable_expected);
									}
								}
								const bool journal_captured =
									cpu_shadow_tag_matches && architectural_state_compared &&
									pending_loop_kernel_shadow_draw->CapturePrivateStoreExpected(
										VU1.Mem, VU1_MEMSIZE);
								const bool attestation_started = journal_captured &&
									VitaGpuVu::BeginGeneratedLoopKernelAttestation(
										attestation_identity);
								if (attestation_started &&
									VitaGS::QueueGpuVuDraw(
										std::move(pending_loop_kernel_shadow_draw)))
								{
								gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
								loop_kernel_shadow_queued = true;
								Console.WriteLn(
									"GPU-VU seq=%llu provider=generated-loop-kernel "
									"gpu_draw=1 output=private-attestation pairs=%u "
									"vertices=%u primitives=%u cpu_oracle_calls=1 "
									"cpu_oracle_pairs=%u cpu_path1_preserved=1 "
										"fragments=disabled depth_writes=disabled "
										"architectural_state_exact=%u state_mismatches=%u "
										"cpu_shadow_playable_match=%u playable_state_mismatches=%u "
										"attestation=private-test product_accepted=0 "
										"validation_canary=%u.",
									static_cast<unsigned long long>(sequence),
									pending_loop_kernel_shadow_pairs, vertices, primitives,
									pending_loop_kernel_shadow_pairs,
										architectural_state_mismatches == 0u ? 1u : 0u,
										architectural_state_mismatches,
										architectural_state_playable_mismatches == 0u ? 1u : 0u,
										architectural_state_playable_mismatches,
									pending_loop_kernel_shadow_validation_canary ? 1u : 0u);
								}
								else
								{
									if (attestation_started)
									{
										VitaGpuVu::CancelGeneratedLoopKernelAttestation(
											attestation_identity);
									}
									else if (attestation_identity.IsValid() &&
										VitaGpuVu::BeginGeneratedLoopKernelAttestation(
											attestation_identity))
									{
										VitaGpuVu::CompleteGeneratedLoopKernelAttestation(
											attestation_identity,
											VitaGpuVu::GeneratedLoopKernelNumericProfile::None,
											architectural_state_compared &&
												architectural_state_mismatches != 0u ?
												VitaGpuVu::GeneratedLoopKernelAttestationRejection::
													ArchitecturalStateMismatch :
												VitaGpuVu::GeneratedLoopKernelAttestationRejection::
													InvalidDescriptor);
									}
									Console.Warning(
									"GPU-VU seq=%llu provider=generated-loop-kernel "
									"gpu_draw=0 reason=%s "
									"root_tag=%08x:%08x:%08x:%08x "
									"cpu_tag=%08x:%08x:%08x:%08x "
									"cpu_path1_preserved=1 product_accepted=0.",
									static_cast<unsigned long long>(sequence),
										!cpu_shadow_tag_matches ?
											"post-execution-output-contract-mismatch" :
										!architectural_state_compared ?
											"architectural-state-compare-failed" :
										!journal_captured ?
											"private-journal-capture-failed" :
											"private-draw-queue-failed",
									gpu_shadow_tag[0], gpu_shadow_tag[1],
									gpu_shadow_tag[2], gpu_shadow_tag[3],
									cpu_shadow_tag[0], cpu_shadow_tag[1],
									cpu_shadow_tag[2], cpu_shadow_tag[3]);
							}
						}
						#endif
						#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						if (!loop_kernel_shadow_queued)
						#endif
						{
							const Common::Timer::Value path1_finish_start =
								VitaPerformanceTelemetry::IsEnabled() ?
									Common::Timer::GetCurrentValue() : 0;
							gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
							if (path1_finish_start != 0)
							{
								VitaGpuVu::RecordUniversalGpuVuCpuPath1FinishTime(
									MtvuElapsedTelemetryMicroseconds(path1_finish_start));
							}
						}
					#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
					if (universal_was_queued &&
						consumed_universal_executes != 0 &&
						universal_epoch->Stage() ==
							VitaGpuVu::UniversalGpuVuEpochStage::GpuRejected)
					{
						// Transactional GPU rejection leaves canonical VU state and
						// PATH1 untouched. Replay every consumed segment now, in the
						// exact queue order, before publishing one CpuFallback record
						// for all of its EE PATH1 reservations.
						vuCycles[vuCycleIdx].store(
							VU1.cycle, std::memory_order_release);
						vuCycleIdx = (vuCycleIdx + 1) & 3;
						for (BufferedGeneratedExecute& execute :
							 buffered_universal_executes)
						{
							for (VitaGpuVu::VifUnpackSpan& span : execute.unpacks)
								AppendDeferredVifUnpack(std::move(span));
							if (execute.addr != -1)
								VU1.VI[REG_TPC].UL =
									static_cast<u32>(execute.addr) & 0x7ffu;
							CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
							VU1.cycle = 0;
							ReplayDeferredVifUnpacks();
							vifRegs.top = execute.vif_top;
							vifRegs.itop = execute.vif_itop;
							BeginProgram();
							const Common::Timer::Value extra_cpu_start =
								VitaPerformanceTelemetry::IsEnabled() ?
									Common::Timer::GetCurrentValue() : 0;
							CpuVU1->Execute(vu1RunCycles);
							if (extra_cpu_start != 0)
							{
								VitaGpuVu::RecordUniversalGpuVuCpuFallbackTime(
									MtvuElapsedTelemetryMicroseconds(extra_cpu_start));
							}
							const u32 extra_path1_bytes =
								gifUnit.gifPath[GIF_PATH_1].gsPack.size;
							VitaGpuVu::RecordCpuVu1Execution(extra_path1_bytes);
							gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
							vuCycles[vuCycleIdx].store(
								VU1.cycle, std::memory_order_release);
							vuCycleIdx = (vuCycleIdx + 1) & 3;
						}
						m_execute_jobs_completed.fetch_add(
							consumed_universal_executes,
							std::memory_order_release);
					}
					#endif
					const Common::Timer::Value completion_publish_start =
						VitaPerformanceTelemetry::IsEnabled() ?
							Common::Timer::GetCurrentValue() : 0;
					#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
					if (universal_was_queued && universal_epoch)
					{
						VitaGpuVu::RecordUniversalGpuVuCpuFallback(
							universal_pair_count);
						pxAssertRel(universal_epoch->MarkCpuFallback(
							universal_rejection, universal_pair_count),
							"universal GPU-VU rejection could not publish CPU fallback");
							VitaGS::NotifyUniversalGpuVuProgress();
							const Common::Timer::Value retirement_start =
								VitaPerformanceTelemetry::IsEnabled() ?
									Common::Timer::GetCurrentValue() : 0;
							while (universal_epoch->Stage() !=
							VitaGpuVu::UniversalGpuVuEpochStage::Retired)
						{
							const auto observed = universal_epoch->Stage();
								universal_epoch->WaitForStageChange(observed);
							}
							if (retirement_start != 0)
							{
								VitaGpuVu::RecordUniversalGpuVuRetirementWait(
									MtvuElapsedTelemetryMicroseconds(retirement_start));
							}
					}
						if (!universal_was_queued && !loop_kernel_shadow_queued)
							VitaGS::CompleteMtvuPath1Packet();
					#else
					VitaGS::CompleteMtvuPath1Packet();
					#endif
						if (completion_publish_start != 0)
						{
							VitaGpuVu::RecordUniversalGpuVuCpuCompletionPublishTime(
								MtvuElapsedTelemetryMicroseconds(
									completion_publish_start));
						}
						#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						if (consumed_universal_executes == 0)
						#endif
						{
							vuCycles[vuCycleIdx].store(
								VU1.cycle, std::memory_order_release);
							vuCycleIdx = (vuCycleIdx + 1) & 3;
						}
						#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						if (universal_attempt_start != 0)
						{
							VitaGpuVu::RecordUniversalGpuVuWorkerAttemptTime(
								MtvuElapsedTelemetryMicroseconds(
									universal_attempt_start));
						}
						#endif
					break;
				}
				case MTVU_VU_WRITE_MICRO:
				{
					u32 vu_micro_addr = Read();
					u32 size = Read();
						Read(&VU1.Micro[vu_micro_addr], size);
						m_gpu_vu_micro_generation++;
						if (m_gpu_vu_micro_generation == 0)
							m_gpu_vu_micro_generation = 1;
						m_gpu_vu_generated_generation_had_product = false;
						m_gpu_vu_dispatch_cost_cache = {};
						m_gpu_vu_dispatch_cost_cache_next = 0;
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
						VitaGpuVuOpportunityCensus::RecordMicroWrite(size);
#endif
						primed_direct_entry_token = 0;
						primed_direct_resume_token = 0;
						active_direct_continuation = {};
						break;
					}
					case MTVU_VU_WRITE_DATA:
					{
						active_direct_continuation = {};
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"VU data write could not drain universal GPU work");
						pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
							"VU data write could not materialize universal GPU state");
						ReplayDeferredVifUnpacks();
					u32 vu_data_addr = Read();
					u32 size = Read();
					Read(&VU1.Mem[vu_data_addr], size);
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					VitaGpuVuOpportunityCensus::RecordDataWrite(size);
#endif
					break;
					}
					case MTVU_VU_WRITE_VIREGS:
						active_direct_continuation = {};
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"VI write could not drain universal GPU work");
						pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
							"VI write could not materialize universal GPU state");
						Read(&VU1.VI, size_u32(32));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
						VitaGpuVuOpportunityCensus::RecordViStateWrite();
#endif
						break;
					case MTVU_VU_WRITE_VFREGS:
						active_direct_continuation = {};
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"VF write could not drain universal GPU work");
						pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
							"VF write could not materialize universal GPU state");
						Read(&VU1.VF, size_u32(4*32));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
						VitaGpuVuOpportunityCensus::RecordVfStateWrite();
#endif
					break;
				case MTVU_VIF_WRITE_COL:
					pxAssertRel(DrainUniversalGpuVuEpochs(true),
						"VIF column write could not drain universal GPU work");
					pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
						"VIF column write could not materialize universal GPU state");
					Read(&vif.MaskCol, sizeof(vif.MaskCol));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					VitaGpuVuOpportunityCensus::RecordColumnStateUpdate();
#endif
					break;
				case MTVU_VIF_WRITE_ROW:
					pxAssertRel(DrainUniversalGpuVuEpochs(true),
						"VIF row write could not drain universal GPU work");
					pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
						"VIF row write could not materialize universal GPU state");
					Read(&vif.MaskRow, sizeof(vif.MaskRow));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					VitaGpuVuOpportunityCensus::RecordRowStateUpdate();
#endif
					break;
					case MTVU_VIF_UNPACK:
					{
						active_direct_continuation = {};
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"VIF UNPACK could not drain universal GPU work");
						pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
							"VIF UNPACK could not materialize universal GPU state");
						ReplayDeferredVifUnpacks();
					u32 vif_copy_size = static_cast<u32>((uptr)&vif.StructEnd - (uptr)&vif.tag);
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					u32 size = Read();
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					VitaGpuVuOpportunityCensus::RecordVifUnpack(
						BuildGpuVuCensusVifSpan(vif, vifRegs, size));
#endif
					MTVU_Unpack(&buffer[m_read_pos], vifRegs);
					m_read_pos += size_u32(size);
					break;
				}
				case MTVU_VIF_UNPACK_CAPTURED:
				{
					VitaGpuVu::VifUnpackSpan span;
					Read(&span, sizeof(span));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
					VitaGpuVuOpportunityCensus::RecordVifUnpack(span);
#endif
					if (!VitaGpuVu::ResolveRawVifPayload(span.payload))
					{
						Console.Error(
							"GPU-VU: immutable VIF payload became invalid before MTVU consumed it.");
						VitaGpuVu::ReleaseRawVifPayload(&span.payload);
						break;
					}
					AppendDeferredVifUnpack(std::move(span));
					break;
				}
					case MTVU_FLUSH_VIF_UNPACKS:
						active_direct_continuation = {};
						pxAssertRel(DrainUniversalGpuVuEpochs(true),
							"VU observer could not drain universal GPU work");
						pxAssertRel(MaterializeUniversalGpuVuStateForCpu(),
							"VU observer could not materialize universal GPU state");
					ReplayDeferredVifUnpacks();
					m_gpu_vu_generated_unpublished_transactions = 0;
					VitaGS::FlushMtvuPath1Completions();
					break;
				case MTVU_NULL_PACKET:
					m_read_pos = 0;
					break;
					jNO_DEFAULT;
			}
			if (mtvu_record_started != 0)
			{
				const u64 record_us =
					MtvuElapsedTelemetryMicroseconds(mtvu_record_started);
				if (completes_vu_execution)
					VitaGpuVu::RecordGeneratedLoopKernelMtvuExecuteRecord(record_us);
				else if (tag == MTVU_VIF_UNPACK_CAPTURED || tag == MTVU_VIF_UNPACK)
					VitaGpuVu::RecordGeneratedLoopKernelMtvuVifRecord(record_us);
				else
					VitaGpuVu::RecordGeneratedLoopKernelMtvuOtherRecord(record_us);
			}

			const Common::Timer::Value mtvu_housekeeping_started =
				VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
				if (completes_vu_execution && !defer_vu_execution_completion)
			{
				m_execute_jobs_completed.fetch_add(
					1, std::memory_order_release);
			}
				CommitReadPos();
				pxAssertRel(DrainUniversalGpuVuEpochs(false),
					"universal GPU-VU completion could not be adopted");
			if (mtvu_housekeeping_started != 0)
			{
				VitaGpuVu::RecordGeneratedLoopKernelMtvuHousekeeping(
					MtvuElapsedTelemetryMicroseconds(mtvu_housekeeping_started));
			}
		}
		// Pair with a producer which arms after the final CommitReadPos().
		// The unconditional release RMW makes that post-drain recheck observe
		// all preceding read-position publications without adding an RMW to
		// every ring record.
		m_ring_space_progress.PublishQuiescence();

		// A fast continuation builder can briefly catch the EE producer between
		// VIF transfers thousands of times per frame. Queue-empty is therefore
		// not an epoch boundary: publishing here fragmented cacheable-to-GXM
		// copies and made CPU0 contend on every short producer burst. Explicit
		// VSync, input-pressure and observation requests close partial runs.
		const u32 publication_requests =
			m_gpu_vu_path1_publication_requests.exchange(
				0u, std::memory_order_acq_rel);
		if ((publication_requests & GpuVuPath1VisibilityRequest) != 0u)
		{
			m_gpu_vu_generated_unpublished_transactions = 0;
			VitaGS::FlushMtvuPath1Completions();
		}
		else if ((publication_requests & GpuVuPath1PublishRequest) != 0u)
		{
			m_gpu_vu_generated_unpublished_transactions = 0;
			VitaGS::PublishMtvuPath1Completions();
		}
		}

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	PublishGpuVuHealth(static_cast<u32>(GpuVuHealthCpu1Stage::Shutdown));
#endif
	m_gpu_vu_generated_unpublished_transactions = 0;
	VitaGS::FlushMtvuPath1Completions();
	m_ring_space_progress.PublishQuiescence();
	semaEvent.Kill();
}


// Should only be called by ReserveSpace()
__ri void VU_Thread::WaitOnSize(s32 size)
{
	const bool performance_telemetry_enabled =
		VitaPerformanceTelemetry::IsEnabled();
	bool counted_wait = false;
	Common::Timer::Value wait_start = 0;
	for (;;)
	{
		s32 readPos = GetReadPos();
		if (readPos <= m_write_pos)
			break; // MTVU is reading in back of write_pos
		// FIXME greg: there is a bug somewhere in the queue pointer
		// management. It creates a deadlock/corruption in SotC intro (before
		// the first menu). I added a 4KB safety net which seem to avoid to
		// trigger the bug.
		// Note: a wait lock instead of a yield also helps to avoid the bug.
		if (readPos > m_write_pos + size + _4kb)
			break; // Enough free front space
		if (m_pending_vif_batch)
		{
			// A transfer larger than the producer ring must expose its earlier
			// complete UNPACK commands before waiting for the worker to make
			// room. This is a pressure escape, not the ordinary hot path.
			PublishPendingVifBatch();
			continue;
		}
		{          // Let MTVU run to free up buffer space
			if (performance_telemetry_enabled && !counted_wait)
			{
				m_profile_ring_waits++;
				counted_wait = true;
				wait_start = Common::Timer::GetCurrentValue();
			}
			KickStart();
			m_ring_space_progress.WaitForChange(readPos, [this]() {
				return GetReadPos();
			});
		}
	}
	if (wait_start != 0)
	{
		VitaGpuVu::RecordUniversalGpuVuCpu0RingWait(
			MtvuElapsedTelemetryMicroseconds(wait_start));
	}
}

// Makes sure theres enough room in the ring buffer
// to write a continuous 'size * sizeof(u32)' bytes
void VU_Thread::ReserveSpace(s32 size)
{
	pxAssert(m_write_pos < buffer_size);
	pxAssert(size < buffer_size);
	pxAssert(size > 0);

	if (m_write_pos + size > (buffer_size - 1))
	{
		WaitOnSize(1); // Size of MTVU_NULL_PACKET
		Write(MTVU_NULL_PACKET);
		// Reset local write pointer/position
		m_write_pos = 0;
		CommitWritePos();
	}

	WaitOnSize(size);
}

// Use this when reading read_pos from ee thread
__fi s32 VU_Thread::GetReadPos()
{
	return m_ato_read_pos.load(std::memory_order_acquire);
}

// Use this when reading write_pos from vu thread
__fi s32 VU_Thread::GetWritePos()
{
	return m_ato_write_pos.load(std::memory_order_acquire);
}

// Gets the effective write pointer after
__fi u32* VU_Thread::GetWritePtr()
{
	pxAssert(m_write_pos < buffer_size);
	return &buffer[m_write_pos];
}

__fi void VU_Thread::CommitWritePos()
{
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		const s32 previous_write_pos =
			m_ato_write_pos.load(std::memory_order_relaxed);
		const u32 queued_words = static_cast<u32>(
			m_write_pos - previous_write_pos) & (buffer_size - 1);
		m_profile_queue_submissions++;
		m_profile_queue_words += queued_words;
	}
	m_ato_write_pos.store(m_write_pos, std::memory_order_release);
	m_pending_vif_batch = false;
	m_pending_captured_vif_span_pos = -1;

	if (MTVU_ALWAYS_KICK)
		KickStart();
	if (MTVU_SYNC_MODE)
		WaitVU();
}

__fi void VU_Thread::CommitReadPos()
{
	m_ato_read_pos.store(m_read_pos, std::memory_order_release);
	m_ring_space_progress.NotifyOfProgress();
}

__fi u32 VU_Thread::Read()
{
	u32 ret = buffer[m_read_pos];
	m_read_pos++;
	return ret;
}

__fi void VU_Thread::Read(void* dest, u32 size)
{
	memcpy(dest, &buffer[m_read_pos], size);
	m_read_pos += size_u32(size);
}

__fi void VU_Thread::ReadRegs(VIFregisters* dest)
{
	VIFregistersMTVU* src = (VIFregistersMTVU*)&buffer[m_read_pos];
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->itop = src->itop;
	dest->top = src->top;
	m_read_pos += size_u32(sizeof(VIFregistersMTVU));
}

__fi void VU_Thread::Write(u32 val)
{
	GetWritePtr()[0] = val;
	m_write_pos += 1;
}

__fi void VU_Thread::Write(const void* src, u32 size)
{
	memcpy(GetWritePtr(), src, size);
	m_write_pos += size_u32(size);
}

__fi void VU_Thread::WriteRegs(VIFregisters* src)
{
	VIFregistersMTVU* dest = (VIFregistersMTVU*)GetWritePtr();
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->top = src->top;
	dest->itop = src->itop;
	m_write_pos += size_u32(sizeof(VIFregistersMTVU));
}

// Returns Average number of vu Cycles from last 4 runs
// Used for vu cycle stealing hack
u32 VU_Thread::Get_vuCycles()
{
	return (vuCycles[0].load(std::memory_order_acquire) +
			vuCycles[1].load(std::memory_order_acquire) +
			vuCycles[2].load(std::memory_order_acquire) +
			vuCycles[3].load(std::memory_order_acquire)) >>
		   2;
}

void VU_Thread::Get_MTVUChanges()
{
	// Note: Atomic communication is with Gif_Unit.cpp Gif_HandlerAD_MTVU
	// The Vita worker publishes completed VU register/memory state before its
	// E/D/T flag. Cortex-A9 needs an acquire here; x86's ordering made the
	// upstream relaxed load sufficient, but it does not establish that contract
	// on a three-core ARM execution route.
	u32 interrupts = mtvuInterrupts.load(std::memory_order_acquire);
	if (!interrupts)
		return;

	if (interrupts & InterruptFlagSignal)
	{
		std::atomic_thread_fence(std::memory_order_acquire);
		const u64 signal = gsSignal.load(std::memory_order_relaxed);
		// If load of signal was moved after clearing the flag, the other thread could write a new value before we load without noticing the double signal
		// Prevent that with release semantics
		mtvuInterrupts.fetch_and(~InterruptFlagSignal, std::memory_order_release);
		GUNIT_WARN("SIGNAL firing");
		const u32 signalMsk = (u32)(signal >> 32);
		const u32 signalData = (u32)signal;
		if (CSRreg.SIGNAL)
		{
			GUNIT_WARN("Queue SIGNAL");
			gifUnit.gsSIGNAL.queued = true;
			//DevCon.Warning("Firing pending signal");
			gifUnit.gsSIGNAL.data[0] = signalData;
			gifUnit.gsSIGNAL.data[1] = signalMsk;
		}
		else
		{
			CSRreg.SIGNAL = true;
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~signalMsk) | (signalData & signalMsk);

			if (!GSIMR.SIGMSK)
				gsIrq();
		}
	}
	if (interrupts & InterruptFlagFinish)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagFinish, std::memory_order_relaxed);
		GUNIT_WARN("Finish firing");
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;

		if (!gifUnit.checkPaths(false, true, true, true))
			Gif_FinishIRQ();
	}
	if (interrupts & InterruptFlagLabel)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagLabel, std::memory_order_acquire);
		// If other thread updates gsLabel for a second interrupt, that's okay.  Worst case we think there's a label interrupt but gsLabel is 0
		// We do not want the exchange of gsLabel to move ahead of clearing the flag, or the other thread could add more work before we clear the flag, resulting in an update with the flag unset
		// acquire semantics should supply that guarantee
		const u64 label = gsLabel.exchange(0, std::memory_order_relaxed);
		GUNIT_WARN("LABEL firing");
		const u32 labelMsk = (u32)(label >> 32);
		const u32 labelData = (u32)label;
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~labelMsk) | (labelData & labelMsk);
	}
	if (interrupts & InterruptFlagVUEBit)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagVUEBit, std::memory_order_relaxed);

		if(INSTANT_VU1)
			VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		//DevCon.Warning("E-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
	}
	if (interrupts & InterruptFlagVUTBit)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagVUTBit, std::memory_order_relaxed);
		VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		VU0.VI[REG_VPU_STAT].UL |= 0x0400;
		//DevCon.Warning("T-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
		hwIntcIrq(7);
	}
}

void VU_Thread::KickStart()
{
	semaEvent.NotifyOfWork();
}

bool VU_Thread::IsDone()
{
	return GetReadPos() == GetWritePos() &&
		!m_pending_vif_batch &&
		m_deferred_vif_unpack_count.load(std::memory_order_acquire) == 0 &&
		m_gpu_vu_universal_pending_epoch_count.load(
			std::memory_order_acquire) == 0 &&
		m_gpu_vu_generated_pending_transaction_count.load(
			std::memory_order_acquire) == 0;
}

void VU_Thread::PublishPendingVifBatch()
{
	if (!m_pending_vif_batch)
		return;
	CommitWritePos();
	KickStart();
}

void VU_Thread::RequestGpuVuPath1Publication()
{
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	m_gpu_vu_path1_publication_requests.fetch_or(
		GpuVuPath1PublishRequest, std::memory_order_release);
	KickStart();
#endif
}

void VU_Thread::RequestGpuVuPath1Flush()
{
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	m_gpu_vu_path1_publication_requests.fetch_or(
		GpuVuPath1PublishRequest | GpuVuPath1VisibilityRequest,
		std::memory_order_release);
	KickStart();
#endif
}

void VU_Thread::WaitForQueue()
{
	PublishPendingVifBatch();
	KickStart();
	semaEvent.WaitForEmpty();
}

void VU_Thread::BoundGpuVuExecutionLatencyAtFrameBoundary()
{
	const bool telemetry_enabled = VitaPerformanceTelemetry::IsEnabled();
	Common::Timer::Value wait_start = 0;
	bool waited = false;
	// A VSync publishes the complete producer ring before applying the ordinary
	// CPU1 latency bound. Generated successors become available when CPU1 queues
	// their descriptors. PostVsyncStart has already requested the ordered
	// visibility close which prevents the VSync from sitting behind a retained
	// generated PATH1 tail; this loop bounds CPU1 execution latency separately.
	PublishPendingVifBatch();
	for (;;)
	{
		u64 enqueued =
			m_execute_jobs_enqueued.load(std::memory_order_relaxed);
		const u64 completed =
			m_execute_jobs_completed.load(std::memory_order_acquire);
		if (completed > enqueued)
		{
			// Reset() may rebuild canonical state while an already-consumed record
			// is still publishing its completion. These are lifetime counters, not
			// guest state; catch the producer baseline up instead of allowing an
			// unsigned outstanding count to become an unbounded wait.
			m_execute_jobs_enqueued.store(completed, std::memory_order_relaxed);
			enqueued = completed;
		}
		if (telemetry_enabled)
		{
			VitaGpuVu::RecordUniversalGpuVuMtvuExecuteOutstanding(
				enqueued - completed);
		}
		if (enqueued - completed < MaximumOutstandingVuExecutions)
			break;

		if (!waited)
		{
			waited = true;
			if (telemetry_enabled)
				wait_start = Common::Timer::GetCurrentValue();
		}
		// The complete frame batch was published before entering this loop. The
		// worker consumes its close request only after reaching queue quiescence,
		// so it remains armed across every record in this published frame.
		KickStart();
		m_ring_space_progress.WaitForChange(completed, [this]() {
			return m_execute_jobs_completed.load(std::memory_order_acquire);
		});
	}

	if (waited && telemetry_enabled)
	{
		VitaGpuVu::RecordUniversalGpuVuCpu0ExecuteBudgetWait(
			MtvuElapsedTelemetryMicroseconds(wait_start));
	}
}

void VU_Thread::WaitVU()
{
	MTVU_LOG("MTVU - WaitVU!");
	if (VitaPerformanceTelemetry::IsEnabled())
		m_profile_wait_calls++;
	if (!IsOpen())
		return;
	const Common::Timer::Value wait_start =
		VitaPerformanceTelemetry::IsEnabled() ?
			Common::Timer::GetCurrentValue() : 0;

	// Public waits are architectural observation boundaries. Queue one ordered
	// materialization command even when the worker has not yet consumed the
	// preceding capture event; checking only the current pending count would
	// race that handoff.
	ReserveSpace(1);
	Write(MTVU_FLUSH_VIF_UNPACKS);
	CommitWritePos();
	WaitForQueue();
	if (wait_start != 0)
	{
		VitaGpuVu::RecordUniversalGpuVuCpu0Wait(
			MtvuElapsedTelemetryMicroseconds(wait_start));
	}
}

void VU_Thread::AppendDeferredVifUnpack(
	VitaGpuVu::VifUnpackSpan span)
{
	// PCSX2 owner: Vif_Unpack.cpp::_nVifUnpackLoop(). Mode-zero V4-32 with
	// CL==WL has no side effect beyond one sequential, wrapping VU-memory
	// write per source vector. A later span which covers that complete modular
	// range therefore makes the older payload unreachable.
	for (auto it = m_deferred_vif_unpacks.begin();
		 it != m_deferred_vif_unpacks.end();)
	{
		if (VitaGpuVu::DirectAffineSpanFullyOverwrites(span, *it))
		{
			VitaGpuVu::ReleaseRawVifPayload(&it->payload);
			it = m_deferred_vif_unpacks.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Partial overlaps require segment splitting to compact further. Preserve
	// exact behavior by materializing the bounded journal instead of allowing
	// unbounded storage or search work on the 496 MHz VU worker.
	if (m_deferred_vif_unpacks.size() >= MaximumDeferredAffineSpans)
		ReplayDeferredVifUnpacks();

	m_deferred_vif_unpacks.push_back(std::move(span));
	m_deferred_vif_unpack_count.store(
		static_cast<u32>(m_deferred_vif_unpacks.size()),
		std::memory_order_release);
	VitaGpuVu::RecordDeferredVifUnpack();
}

void VU_Thread::ReplayDeferredVifUnpacks()
{
	if (!DrainUniversalGpuVuEpochs(true))
	{
		Console.Error(
			"GPU-VU: refusing deferred VIF replay before pending epochs drain.");
		return;
	}
	if (!m_deferred_vif_unpacks.empty() &&
		!MaterializeUniversalGpuVuStateForCpu())
	{
		Console.Error(
			"GPU-VU: refusing deferred VIF replay from a stale CPU VU1 state.");
		return;
	}
	ReplayVifUnpackSpans(&m_deferred_vif_unpacks);
	m_deferred_vif_unpack_count.store(0, std::memory_order_release);
}

void VU_Thread::ReplayVifUnpackSpans(
	std::vector<VitaGpuVu::VifUnpackSpan>* spans)

{
	if (!spans)
		return;
	for (VitaGpuVu::VifUnpackSpan& span : *spans)
	{
		const u8* const source =
			VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!source)
		{
			Console.Error(
				"GPU-VU: deferred VIF payload was unavailable at its observation boundary.");
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continue;
		}

		if (VitaGpuVu::MaterializeDirectAffineV4_32Span(
				span, source, VU1.Mem, VU1_MEMSIZE))
		{
			VitaGpuVu::RecordReplayedVifUnpack();
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continue;
		}

		// General completed UNPACKs are retained for the universal GPU command
		// chain. At a real CPU observation or GPU rejection, replay every other
		// supported descriptor through PCSX2's owning MTVU unpack mechanism.
		vif.tag.addr = static_cast<u32>(span.destination_qword) * 16u;
		vif.tag.size = span.tag_size_words;
		vif.tag.cmd = span.command;
		vif.cmd = span.command;
		vif.pass = 1;
		vif.cl = 0;
		vif.usn = span.unsigned_data;
		vif.start_aligned = span.start_alignment;
		vifRegs.cycle.cl = span.cycle_cl;
		vifRegs.cycle.wl = span.cycle_wl;
		vifRegs.mode = span.mode;
		vifRegs.num = span.vector_count;
		vifRegs.mask = span.mask;
		vifRegs.top = span.vif_top;
		vifRegs.itop = span.vif_itop;
		MTVU_Unpack(const_cast<u8*>(source), vifRegs);
		VitaGpuVu::RecordReplayedVifUnpack();
		VitaGpuVu::ReleaseRawVifPayload(&span.payload);
	}
	spans->clear();
}

bool VU_Thread::MaterializeUniversalGpuVuStateForCpu()
{
	if (m_gpu_vu_universal_committed_sequence == 0)
		return true;
	const VitaGpuVu::UniversalGpuVuCommittedStateView view{
		m_gpu_vu_universal_committed_vf,
		m_gpu_vu_universal_committed_state,
		m_gpu_vu_universal_committed_memory,
		m_gpu_vu_universal_committed_sequence};
	if (!VitaGpuVu::MaterializeUniversalGpuVuCommittedState(view, &VU1))
	{
		Console.Error(
			"GPU-VU: failed to materialize committed sequence %llu at a CPU ownership boundary.",
			static_cast<unsigned long long>(
				m_gpu_vu_universal_committed_sequence));
		return false;
	}
	for (u32 lane = 0; lane < 4; lane++)
	{
		vif.MaskRow._u32[lane] =
			view.state_words[VitaGpuVu::UniversalGpuVuStateVifRowWord + lane];
		vif.MaskCol._u32[lane] =
			view.state_words[VitaGpuVu::UniversalGpuVuStateVifColumnWord + lane];
	}
	m_gpu_vu_universal_committed_sequence = 0;
	m_gpu_vu_universal_committed_tpc = 0;
	m_gpu_vu_universal_committed_vf = nullptr;
	m_gpu_vu_universal_committed_state = nullptr;
	m_gpu_vu_universal_committed_memory = nullptr;
	return true;
}

void VU_Thread::ReleaseDeferredVifUnpacks()
{
	for (VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
		VitaGpuVu::ReleaseRawVifPayload(&span.payload);
	m_deferred_vif_unpacks.clear();
	m_deferred_vif_unpack_count.store(0, std::memory_order_release);
}

void VU_Thread::ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst)
{
	MTVU_LOG("MTVU - ExecuteVU!");
	PrepareVuCodeForExecute(static_cast<s32>(vu_addr));
	Get_MTVUChanges(); // Clear any pending interrupts
	bool direct_program_job = false;
	bool prime_program_job = false;
	VitaGpuVu::DirectProgramToken execution_direct_program{};
	VitaGpuVu::DirectProgramToken continuation_direct_program{};
	if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
		vu_addr == static_cast<u32>(-1) &&
		m_gpu_vu_direct_resume_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_resume_token});
		if (input_state == VitaGpuVu::DirectInputState::Pending ||
			input_state == VitaGpuVu::DirectInputState::Ready)
		{
			direct_program_job = true;
			execution_direct_program = {m_gpu_vu_direct_resume_token};
			continuation_direct_program = {m_gpu_vu_direct_program_token};
		}
		else
		{
			m_gpu_vu_direct_resume_token = 0;
		}
	}
	else if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
		vu_addr != static_cast<u32>(-1) &&
		m_gpu_vu_direct_program_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_program_token});
			if (input_state == VitaGpuVu::DirectInputState::Pending)
			{
				// The explicit root contains the skipped MSCAL prologue and is the
				// eventual GPU-owned continuation seed. Compile it now, but keep the
				// CPU execution authoritative until its live VF/VI state can be
				// carried across following MSCNT commands without a stale snapshot.
				prime_program_job = true;
				execution_direct_program = {m_gpu_vu_direct_program_token};
			}
			else if (input_state == VitaGpuVu::DirectInputState::Ready &&
				m_gpu_vu_direct_resume_token != 0 &&
				VitaGpuVu::GetDirectProgramInputState(
					{m_gpu_vu_direct_resume_token}) ==
						VitaGpuVu::DirectInputState::Ready)
			{
				// Both exact-image entries are registered. The worker performs
				// the semantic same-loop continuation proof before it may replace
				// this explicit MSCAL with the generated entry root.
				direct_program_job = true;
				execution_direct_program = {m_gpu_vu_direct_program_token};
				continuation_direct_program = {
					m_gpu_vu_direct_resume_token};
			}
		}
	const bool tagged_program_job =
		direct_program_job || prime_program_job;
	ReserveSpace(tagged_program_job ? 9 : 7);
	Write(direct_program_job ? MTVU_VU_EXECUTE_DIRECT :
		(prime_program_job ? MTVU_VU_EXECUTE_DIRECT_PRIME :
			MTVU_VU_EXECUTE));
	Write(vu_addr);
	Write(vif_top);
	Write(vif_itop);
	Write(fbrst);
	const u64 execute_enqueued_at = VitaPerformanceTelemetry::IsEnabled() ?
		Common::Timer::GetCurrentValue() : 0;
	Write(static_cast<u32>(execute_enqueued_at));
	Write(static_cast<u32>(execute_enqueued_at >> 32));
	// MSCNT resumes from the MTVU-owned TPC. The token is prepared from the
	// exact post-E PC, never guessed from CPU0's deliberately stale TPC. An
	// explicit MSCAL carries both exact entry and continuation tokens so the
	// worker can prove and seed a GPU-owned chain before bypassing ARM work.
	if (tagged_program_job)
	{
		Write(execution_direct_program.value);
		Write(continuation_direct_program.value);
	}
	m_execute_jobs_enqueued.fetch_add(1, std::memory_order_release);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (!tagged_program_job)
	{
		// Keep an ordinary Execute private until VIF1's transfer boundary.
		// Captured UNPACKs and following MSCAL/MSCNT records then become one
		// immutable no-observer stream which the worker can preflight as a
		// multi-Execute universal epoch. WaitVU(), ring pressure, and frame
		// latency bounds all call PublishPendingVifBatch(), so this never
		// weakens an architectural observation boundary.
		m_pending_vif_batch = true;
		m_pending_captured_vif_span_pos = -1;
	}
	else
#endif
	{
		CommitWritePos();
	}
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		m_profile_execute_enqueues++;
		const u32 used_words = static_cast<u32>(
			(m_write_pos - GetReadPos()) & (buffer_size - 1));
		VitaGpuVu::RecordUniversalGpuVuMtvuQueueUsedWords(used_words);
	}
	gifUnit.TransferGSPacketData(GIF_TRANS_MTVU, NULL, 0);
	KickStart();
	u32 cycles = std::max(Get_vuCycles(), 4u);
	u32 skip_cycles = std::min(cycles, 3000u);
	cpuRegs.cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	VU0.cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	Get_MTVUChanges();

	if (!INSTANT_VU1)
	{
		VU0.VI[REG_VPU_STAT].UL |= 0x100;
		CPU_INT(VU_MTVU_BUSY, cycles);
	}
}

void VU_Thread::VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size)
{
	MTVU_LOG("MTVU - VifUnpack!");
#if defined(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY) || \
	defined(VITASX2_GPU_VU_DIRECT_ADMISSION) || \
	(defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	 VITASX2_GPU_VU_UNIVERSAL_VALIDATION)
	bool retain_immutable_input = false;
#if defined(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY)
	retain_immutable_input = true;
#else
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	// The rejected fixed/structured product providers must not make the cold
	// CPU path copy and replay every UNPACK.  A future generated loop-kernel
	// provider opens capture explicitly only after its immutable analysis is
	// ready and profitable.
	retain_immutable_input =
		VitaGpuVu::IsGeneratedLoopKernelProductAdmissionEnabled();
#endif
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	// Preserve the already-proven specialized generated route.  It needs raw
	// input only after the exact entry/resume root is registered; compilation-
	// pending and unavailable programs retain the ordinary MTVU transfer.
	if (m_gpu_vu_direct_resume_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_resume_token});
		if (input_state == VitaGpuVu::DirectInputState::Ready)
		{
			retain_immutable_input = true;
		}
		else if (input_state == VitaGpuVu::DirectInputState::Unavailable)
		{
			VitaGpuVu::RecordCaptureBypass(size);
			m_gpu_vu_direct_resume_token = 0;
		}
	}
#endif
#endif
	if (retain_immutable_input)
	{
		VitaGpuVu::VifUnpackSpan span;
		span.sequence = ++m_vif_span_sequence;
		span.source_size = size;
		span.tag_size_words = _vif.tag.size;
		span.mask = _vifRegs.mask;
		span.destination_qword = static_cast<u16>(_vif.tag.addr >> 4);
		span.vector_count = static_cast<u16>(_vifRegs.num);
		span.vif_top = static_cast<u16>(_vifRegs.top);
		span.vif_itop = static_cast<u16>(_vifRegs.itop);
		span.command = static_cast<u8>(_vif.tag.cmd);
		span.cycle_cl = _vifRegs.cycle.cl;
		span.cycle_wl = _vifRegs.cycle.wl;
		span.mode = static_cast<u8>(_vifRegs.mode);
		span.unsigned_data = _vif.usn;
		span.start_alignment = _vif.start_aligned;
		u32 universal_payload_size = 0;
		if (VitaGpuVu::GetUniversalVifUnpackPayloadSize(
				span, &universal_payload_size) &&
			universal_payload_size <= size)
		{
			// nVifUnpack() supplies a trailing safety word for generated V3/V4
			// loads.  The PCSX2-derived descriptor computes the exact maximum
			// byte read for every valid format, CL/WL mode and vector count; retain
			// only that immutable extent in the mapped epoch ring.
			span.source_size = universal_payload_size;
			if (VitaGpuVu::CaptureRawVifPayload(
				data, universal_payload_size,
				m_pending_vif_batch ?
					VitaGpuVu::RawVifCaptureMode::ContinueEpoch :
					VitaGpuVu::RawVifCaptureMode::BeginVuCommandEpoch,
				&span.payload))
			{
				// The record is still EE-private until CommitWritePos().
				// Adjacent direct-affine source and destination ranges have the
				// exact effect of one larger journal span. Other formats remain
				// separate ordered records for the universal VIF kernel.
				if (VitaGpuVu::IsDirectAffineV4_32Span(span) &&
					m_pending_vif_batch &&
					m_pending_captured_vif_span_pos >= 0)
				{
					VitaGpuVu::VifUnpackSpan previous;
					std::memcpy(&previous,
						&buffer[m_pending_captured_vif_span_pos],
						sizeof(previous));
					if (VitaGpuVu::MergeAdjacentDirectAffineV4_32Spans(
							&previous, &span))
					{
						std::memcpy(
							&buffer[m_pending_captured_vif_span_pos],
							&previous, sizeof(previous));
						return;
					}
				}

				ReserveSpace(1 + size_u32(sizeof(span)));
				Write(MTVU_VIF_UNPACK_CAPTURED);
				m_pending_captured_vif_span_pos = m_write_pos;
				Write(&span, sizeof(span));
				// The common VIF packet immediately follows its captured UNPACKs
				// with MSCNT. ExecuteVU() publishes the whole group with one
				// release store and one worker wake. VIF1transfer() publishes at
				// return when no execute follows, preserving ordinary MTVU
				// visibility and every observation boundary.
				m_pending_vif_batch = true;
				return;
			}
		}
	}
#endif

	u32 vif_copy_size = (u32)((uptr)&_vif.StructEnd - (uptr)&_vif.tag);
	ReserveSpace(1 + size_u32(vif_copy_size) + size_u32(sizeof(VIFregistersMTVU)) + 1 + size_u32(size));
	Write(MTVU_VIF_UNPACK);
	Write(&_vif.tag, vif_copy_size);
	WriteRegs(&_vifRegs);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteMicroMem!");
	if (size != 0)
	{
		m_gpu_vu_direct_program_prepared = false;
		m_gpu_vu_direct_program_token = 0;
		m_gpu_vu_direct_resume_token = 0;
		m_gpu_vu_direct_program_configuration_bits = 0;
		const u32 end = std::min<u32>(vu_micro_addr + size, VU1_PROGSIZE);
		if (!m_micro_write_pending)
		{
			m_micro_invalidate_start = vu_micro_addr;
			m_micro_invalidate_end = end;
			m_micro_write_pending = true;
		}
		else
		{
			m_micro_invalidate_start = std::min(m_micro_invalidate_start, vu_micro_addr);
			m_micro_invalidate_end = std::max(m_micro_invalidate_end, end);
		}
	}
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_MICRO);
	Write(vu_micro_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::PrepareVuCodeForExecute(s32 vu_addr)
{
	const bool native_preparation_required =
		m_micro_write_pending || VitaVU::Vu1ProgramNeedsPreparation(vu_addr);
	const bool direct_admission_connected =
		VitaGpuVu::IsDirectDrawAdmissionConnected() &&
		VitaGpuVu::IsGeneratedLoopKernelProductAdmissionEnabled();
	const bool direct_start_known =
		direct_admission_connected && vu_addr != -1;
	const u32 direct_start_pc = direct_start_known ?
		((static_cast<u32>(vu_addr) & 0x7ffu) << 3) : 0;
	const u32 direct_configuration_bits = direct_start_known ?
		VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits() : 0;
	if (!native_preparation_required &&
		(!direct_start_known ||
			(m_gpu_vu_direct_program_prepared &&
				m_gpu_vu_direct_program_start_pc == direct_start_pc &&
				m_gpu_vu_direct_program_configuration_bits ==
					direct_configuration_bits)))
		return;

	// Sony PSP2 VM-domain write mode applies to the process, not one core. The
	// PCSX2 x86 MTVU worker may compile at first use, but doing so on Vita races
	// CPU0's executable EE cache. Drain pending micro writes, invalidate and
	// compile on the EE-side C++ seam, then publish only executable code to CPU1.
	WaitForQueue();
	if (VitaPerformanceTelemetry::IsEnabled())
		m_profile_compile_barriers++;
	if (m_micro_write_pending)
	{
		CpuVU1->Clear(m_micro_invalidate_start,
			m_micro_invalidate_end - m_micro_invalidate_start);
		m_micro_write_pending = false;
		m_micro_invalidate_start = 0;
		m_micro_invalidate_end = 0;
	}
	if (native_preparation_required)
		VitaVU::PrepareVu1Program(vu_addr);

	if (!direct_start_known)
	{
		// MSCNT has no explicit VIF address, but the exact post-E token was
		// proven from the preceding explicit program and remains valid across
		// preparation of the native fallback's live resume map. Micro writes,
		// Reset(), and a later explicit MSCAL own invalidation/replacement.
		if (!direct_admission_connected)
		{
			m_gpu_vu_direct_program_token = 0;
			m_gpu_vu_direct_resume_token = 0;
			m_gpu_vu_direct_program_configuration_bits = 0;
			m_gpu_vu_direct_program_prepared = false;
		}
		return;
	}

	const VitaGpuVu::DirectProgramToken prepared =
		VitaGpuVu::PrepareDirectProgramForConfiguration(
			VU1.Micro, VU1_PROGSIZE, direct_start_pc,
			direct_configuration_bits);
	VitaGpuVu::DirectProgramInfo info;
	m_gpu_vu_direct_program_token =
		prepared.IsValid() &&
			VitaGpuVu::GetDirectProgramInfo(prepared, &info) &&
			info.parallel_candidates != 0 ?
		prepared.value : 0;
	m_gpu_vu_direct_resume_token = 0;
	VitaGpuVu::DirectProgramInfo resume_info;
	VitaGpuVu::DirectProgramToken resume{0};
	if (prepared.IsValid() && info.resume_pc_count == 1)
	{
		resume = VitaGpuVu::PrepareDirectProgramForConfiguration(
			VU1.Micro, VU1_PROGSIZE, info.unique_resume_pc,
			direct_configuration_bits);
		if (resume.IsValid() &&
			VitaGpuVu::GetDirectProgramInfo(resume, &resume_info) &&
			resume_info.parallel_candidates != 0 &&
			resume_info.resume_pc_count == 1 &&
			resume_info.unique_resume_pc == info.unique_resume_pc)
		{
			m_gpu_vu_direct_resume_token = resume.value;
		}
	}
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	// Bounded publication evidence: preparation only reaches here on a real
	// derivation, and the report is further limited to observed changes.
	if (m_gpu_vu_direct_program_token != m_gpu_vu_direct_reported_program_token ||
		m_gpu_vu_direct_resume_token != m_gpu_vu_direct_reported_resume_token)
	{
		m_gpu_vu_direct_reported_program_token = m_gpu_vu_direct_program_token;
		m_gpu_vu_direct_reported_resume_token = m_gpu_vu_direct_resume_token;
		Console.WriteLn(
			"GPU-VU: prepare entry %04x token %08x (%u candidates, %u resume "
			"pcs, resume pc %04x) -> resume token %08x (raw %08x, %u "
			"candidates, %u resume pcs, resume pc %04x).",
			direct_start_pc, m_gpu_vu_direct_program_token,
			info.parallel_candidates, info.resume_pc_count,
			info.unique_resume_pc, m_gpu_vu_direct_resume_token, resume.value,
			resume_info.parallel_candidates, resume_info.resume_pc_count,
			resume_info.unique_resume_pc);
	}
#endif
	m_gpu_vu_direct_program_start_pc = direct_start_pc;
	m_gpu_vu_direct_program_configuration_bits = direct_configuration_bits;
	m_gpu_vu_direct_program_prepared = true;
}

void VU_Thread::WriteDataMem(u32 vu_data_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteDataMem!");
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_DATA);
	Write(vu_data_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteVIRegs(REG_VI* viRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32));
	Write(MTVU_VU_WRITE_VIREGS);
	Write(viRegs, size_u32(32));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteVFRegs(VECTOR* vfRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32*4));
	Write(MTVU_VU_WRITE_VFREGS);
	Write(vfRegs, size_u32(32*4));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteCol(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteCol!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskCol)));
	Write(MTVU_VIF_WRITE_COL);
	Write(&_vif.MaskCol, sizeof(_vif.MaskCol));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteRow(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteRow!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskRow)));
	Write(MTVU_VIF_WRITE_ROW);
	Write(&_vif.MaskRow, sizeof(_vif.MaskRow));
	CommitWritePos();
	KickStart();
}
