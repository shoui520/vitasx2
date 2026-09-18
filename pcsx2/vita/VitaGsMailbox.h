// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <memory>

class Error;
namespace VitaGpuVu
{
	class GpuVuDraw;
	class UniversalGpuVuEpoch;
	struct RawVifPayloadRef;
}

namespace VitaGS
{
	enum class GpuVuGxmSubmissionStage : u32
	{
		DrawEncoding = 1,
		VertexProgramBinding,
		FragmentProgramBinding,
		TextureBinding,
		RasterStateBinding,
		FinalRasterBinding,
		VertexDefaultUniformReservation,
		VertexDefaultUniformUpload,
		FragmentDefaultUniformReservation,
		FragmentDefaultUniformUpload,
		AuxiliaryUniformBinding,
		PrecomputeProgramBinding,
		FinalProgramRestore,
		DrawBinding,
		DrawCall,
		DrawReturned,
		TransactionClaim,
		ResourceOwnershipTransfer,
		ResourcesRetained,
		RenderReturnedToDevice,
		SceneDrawAccounting,
		SceneInputOwnership,
		DescriptorInputRelease,
		DescriptorRelease,
		DrawsRetainedForScene,
		DeviceReturn,
		GsStateGroupBegin,
		GsStateGroupReturned,
		GsStateGroupAdvanced,
		GsStateContractAdvanced,
		GsStateConsumeReturned,
		MtgsConsumeReturned,
		MtgsCpuPath1Replay,
		MtgsCpuPath1ReplayReturned,
		MtgsSubmissionMarkerCheck,
		MtgsCommandReturn,
		MidSceneFlush,
		EndScene,
		SceneTransition,
		BeginScene,
		FinishDrain,
		Count,
	};

	// Identifies the exact libGXM call boundary currently owned by MTGS.  This
	// is deliberately separate from GpuVuGxmSubmissionStage: a malformed
	// generated command can poison the context and make a later ordinary GS
	// draw the first call which does not return.
	enum class GpuVuGxmCallKind : u32
	{
		GeneratedPrecomputeDraw = 1,
		GeneratedBatchDraw,
		GeneratedDirectDraw,
		OrdinaryTfxDraw,
		ClearDraw,
		ScissorDraw,
		PresentationDraw,
		MidSceneFlush,
		EndScene,
		BeginScene,
		Finish,
		Count,
	};

	struct GpuVuGxmCallBreadcrumb
	{
		u64 sequence_end = 0;
		u64 last_generated_scene = 0;
		u64 last_generated_sequence = 0;
		u64 fragment_completed_scene = 0;
		u64 fragment_completed_generated_scene = 0;
		u64 fragment_completed_generated_sequence = 0;
		u64 fragment_oldest_scene = 0;
		u64 fragment_oldest_generated_scene = 0;
		u64 fragment_oldest_generated_sequence = 0;
		u64 program_key_high = 0;
		u64 program_key_low = 0;
		uptr input_owner = 0;
		uptr output_address = 0;
		uptr notification_address = 0;
		uptr fragment_notification_address = 0;
		uptr scene_render_target = 0;
		uptr scene_depth_target = 0;
		uptr vertex_program = 0;
		uptr fragment_program = 0;
		uptr source_texture_object = 0;
		uptr source_texture_descriptor = 0;
		uptr source_texture_data = 0;
		uptr render_target_data = 0;
		uptr depth_target_data = 0;
		uptr depth_stencil_data = 0;
		uptr buffer0_address = 0;
		uptr buffer1_address = 0;
		uptr buffer2_address = 0;
		uptr buffer3_address = 0;
		uptr buffer4_address = 0;
		uptr index_address = 0;
		u64 ps_selector_low = 0;
		u64 ps_selector_high = 0;
		u64 buffer0_hash = 0;
		u64 buffer1_hash = 0;
		u64 buffer4_hash = 0;
		u64 index_hash = 0;
		u32 input_slot = ~u32{0};
		u32 input_generation = 0;
		u32 input_first_qword = 0;
		u32 input_last_qword = 0;
		u32 retirement_slot = ~u32{0};
		u32 program_abi = 0;
		u32 phase_index = ~u32{0};
		u32 module_index = ~u32{0};
		u32 object_index = ~u32{0};
		u32 group_index = ~u32{0};
		u32 index_count = 0;
		u32 index_minimum = 0;
		u32 index_maximum = 0;
		u32 object_count = 0;
		u32 private_transaction_count = 0;
		u32 output_bytes = 0;
		u32 output_allocation_bytes = 0;
		u32 output_guard_offset_bytes = 0;
		u32 output_maximum_write_word = 0;
		u32 output_payload_capacity_words = 0;
		u32 output_probe_maximum_write_word = 0;
		u32 output_probe_capacity_words = 0;
		u32 notification_value = 0;
		u32 notification_observed = 0;
		u32 fragment_notification_value = 0;
		u32 fragment_notification_observed = 0;
		u32 fragment_completed_value = 0;
		u32 fragment_last_submitted_value = 0;
		u32 fragment_oldest_value = 0;
		u32 fragment_oldest_draw_call_count = 0;
		u32 fragment_oldest_object_count = 0;
		u32 fragment_oldest_flags = 0;
		u32 scenes_since_generated = 0;
		u32 scene_flags = 0;
		u32 primitive_type = 0;
		u32 topology = 0;
		u32 sampler_key = 0;
		u32 blend_key = 0;
		u32 color_mask_key = 0;
		u32 depth_key = 0;
		u32 texture_type = 0;
		u32 texture_format = 0;
		u32 texture_width = 0;
		u32 texture_height = 0;
		u32 texture_stride = 0;
		u32 texture_mipmap_count = 0;
		u32 texture_sampler_state = 0;
		u32 source_texture_storage_bytes = 0;
		u32 render_target_storage_bytes = 0;
		u32 depth_target_storage_bytes = 0;
		u32 depth_stencil_storage_bytes = 0;
		u32 buffer0_bytes = 0;
		u32 buffer1_bytes = 0;
		u32 buffer2_bytes = 0;
		u32 buffer3_bytes = 0;
		u32 buffer4_bytes = 0;
		u32 index_bytes = 0;
		u32 buffer0_hash_bytes = 0;
		u32 buffer1_hash_bytes = 0;
		u32 buffer4_hash_bytes = 0;
		u32 index_hash_bytes = 0;
		u32 resource_overlap_mask = 0;
		u32 draw_flags = 0;
	};

	struct GpuVuGxmSubmissionBreadcrumb
	{
		uptr input_owner = 0;
		uptr output_address = 0;
		u32 input_slot = ~u32{0};
		u32 input_generation = 0;
		u32 input_payload_count = 0;
		u32 stream_count = 0;
		u32 object_count = 0;
		u32 private_transaction_count = 0;
		u32 output_bytes = 0;
		u32 retirement_slot = ~u32{0};
	};

	// Identifies progress after a generated draw has returned from GSDeviceGXM.
	// These indices remain pointer-free so the independent notification owner
	// can report the exact CPU2 group/command edge even when MTGS stops making
	// progress before the scene obtains a completion notification.
	struct GpuVuGxmConsumerBreadcrumb
	{
		u32 total_draws = 0;
		u32 state_first = 0;
		u32 state_end = 0;
		u32 group_first = 0;
		u32 group_end = 0;
		u32 group_index = 0;
		u32 flags = 0;
	};

	enum class GpuVuGxmOwnershipAction : u32
	{
		None = 0,
		DrawAccounting,
		InputInspect,
		InputAlreadyRetained,
		InputTransferredToScene,
		DescriptorInputReleaseBefore,
		DescriptorInputReleaseAfter,
		DescriptorDestroyBefore,
		DescriptorDestroyAfter,
		Complete,
		Count,
	};

	// Pointer-free progress through the CPU2 ownership handoff which follows a
	// successful sceGxmDraw(). The notification thread samples this storage, so
	// a blocked reference release or descriptor destructor remains visible even
	// when MTGS and libGXM can no longer produce ordinary telemetry.
	struct GpuVuGxmOwnershipBreadcrumb
	{
		uptr input_owner = 0;
		u32 input_slot = ~u32{0};
		u32 input_generation = 0;
		u32 input_offset = 0;
		u32 input_size = 0;
		u32 input_references = 0;
		u32 draw_index = ~u32{0};
		u32 input_index = ~u32{0};
		u32 retained_input_count = 0;
		u32 descriptor_count = 0;
		GpuVuGxmOwnershipAction action = GpuVuGxmOwnershipAction::None;
	};

	// A generated transaction becomes irreversible when CPU1 publishes its
	// direct-output descriptor.  Track the ordered hand-off before libGXM owns a
	// completion notification so a lost mailbox marker cannot strand CPU1
	// outside the GXM submission watchdog's observable interval.
	enum class GeneratedGpuVuHandoffStage : u32
	{
		CompletionPublish = 1,
		CompletionPublished,
		MtgsConsumed,
		DrawsEncoded,
		SubmissionRequested,
	};

	void SetNativePresenterEnabled(bool enabled);
	bool IsNativePresenterEnabled();
	// Mirrors PCSX2's trace-start frame ownership at
	// VMManager::EntryPointCompilingOnCPUThread(). Performance windows after this
	// call use the game ELF entry as their VSync origin.
	void NotifyPerformanceElfEntry();
	// Starts the same warm-up/window cadence at a successfully loaded product
	// workload state, without mislabelling the resulting receipt as ELF entry.
	void NotifyPerformanceWorkloadReplayLoaded();

	// Completes the next EE-reserved MTVU PATH1 ordering point with an immutable
	// direct draw. Ownership returns false by destruction; a successful call is
	// consumed exactly once by the GS/GXM-owning thread.
	bool QueueGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);

	// Publishes one preflight-complete universal transaction at the next MTVU
	// PATH1 ordering point. MTVU retains ownership until the GS mailbox marks
	// the descriptor Retired; no canonical state or output is transferred by
	// this pointer publication.
	bool QueueUniversalGpuVuEpoch(VitaGpuVu::UniversalGpuVuEpoch* epoch);
	// Completion notifications are bridged asynchronously back to the ordered
	// GS owner. No execution owner waits on an ordinary epoch or continuation.
	void NotifyUniversalGpuVuProgress();
	// Diagnostic isolation for the no-coredump generated GXM stall. When
	// enabled, the GS owner waits at the fragment notification attached to each
	// generated scene before encoding a later scene. This changes queue depth,
	// never PS2 state, and is deliberately disabled by default.
	void SetGpuVuFragmentCompletionIsolationEnabled(bool enabled);
	bool IsGpuVuFragmentCompletionIsolationEnabled();
	// Arms the process-lifetime notification bridge for a vertex-only GXM job.
	// This is used by generated transactions whose architectural successor
	// state must become visible before the guest can reach the scene-ending
	// VSync.  The bridge only wakes MTGS; it never waits for or touches GXM.
	bool ArmGpuVuVertexNotification(
		uptr address, u32 value, u64 sequence);

	// Covers the interval in which generated GPU-VU commands have entered the
	// immediate context but no completion notification can yet be observed.
	// The returned ticket is process-local and must be completed only after the
	// libGXM submission call returns or an asynchronous notification is armed.
	// A two-second no-progress interval terminates VitaSX2 only; it never resets
	// the device or releases mappings whose firmware ownership is unknown.
	u32 ArmGpuVuGxmSubmissionWatchdog(
		GpuVuGxmSubmissionStage stage, u64 sequence, u64 scene_serial);
	void UpdateGpuVuGxmSubmissionWatchdog(
		u32 ticket, GpuVuGxmSubmissionStage stage, u64 sequence,
		u64 scene_serial);
	// Advances the currently active pre-notification ticket without exposing
	// the GSDeviceGXM-owned ticket to the GS state/MTGS consumers. A call made
	// after notification activation is intentionally a no-op.
	void UpdateActiveGpuVuGxmSubmissionWatchdog(
		GpuVuGxmSubmissionStage stage);
	void UpdateActiveGpuVuGxmSubmissionWatchdog(
		GpuVuGxmSubmissionStage stage,
		const GpuVuGxmConsumerBreadcrumb& breadcrumb);
	void UpdateGpuVuGxmSubmissionWatchdogBreadcrumb(u32 ticket,
		const GpuVuGxmSubmissionBreadcrumb& breadcrumb);
	void UpdateGpuVuGxmSubmissionWatchdogOwnership(u32 ticket,
		const GpuVuGxmOwnershipBreadcrumb& breadcrumb);
	void UpdateGpuVuGxmSubmissionWatchdogRetirementSlot(
		u32 ticket, u32 retirement_slot);
	void CompleteGpuVuGxmSubmissionWatchdog(u32 ticket);

	// Records the exact before/after boundary of calls which can submit or wait
	// on GXM.  The sampled journal is persisted through the normal VitaSX2 log;
	// the notification owner independently reports a call which remains inside
	// libGXM for 250 ms.  Tokens are process-local and single-owner (MTGS).
	u32 BeginGpuVuGxmCall(GpuVuGxmCallKind kind, u64 sequence,
		u64 scene_serial, const GpuVuGxmCallBreadcrumb& breadcrumb = {});
	void CompleteGpuVuGxmCall(u32 token, int result);

	// Arms one process-wide watchdog for CPU1's strong generated-transaction
	// drain. There is one MTVU producer and only one such drain can be active.
	// MTGS advances the stage as it consumes the ordered completion and clears
	// the watchdog after a vertex notification has been submitted/armed. A
	// timeout terminates VitaSX2 only and deliberately retains kernel-owned GXM
	// mappings for process teardown.
	bool ArmGeneratedGpuVuHandoffWatchdog(
		GeneratedGpuVuHandoffStage stage, u64 sequence);
	void UpdateGeneratedGpuVuHandoffWatchdog(
		GeneratedGpuVuHandoffStage stage, u64 sequence);
	void CompleteGeneratedGpuVuHandoffWatchdog(u64 sequence);

	// Completes the next EE-reserved MTVU PATH1 ordering point with the packet
	// already published by Gif_Path::FinishGSPacketMTVU(). This replaces the
	// desktop per-dispatch semaXGkick lifecycle on the Vita mailbox.
	void CompleteMtvuPath1Packet();

	// Completes a committed generated no-output execution, in the same stream
	// as direct draws, without a CPU GIF packet or a per-execution GXM flush.
	void CompleteMtvuPath1NoOutput();

	// Publishes a direct-descriptor prefix into the current ordered GXM scene
	// without forcing a vertex-only firmware job. The final generated descriptor
	// stays private until a later strong boundary supplies its submit marker.
	u32 PublishMtvuPath1Completions();

	// Publishes a trailing run of direct descriptors before a strong MTVU
	// observation/drain which may have no following CPU PATH1 packet, and asks
	// GXM for early vertex visibility when a generated transaction is present.
	void FlushMtvuPath1Completions();

	// Wakes the GS owner after the asynchronous compiler publishes a completed
	// GXP. This is a CPU work notification only: it never waits for or flushes
	// GXM, and registration/patching still occurs exclusively on the GS thread.
	void NotifyGpuVuCompilerResult();

	// Wakes the GS owner when immutable VIF capture has exhausted all four
	// mapped slots. The owner alone may submit/wait for GXM retirement; the
	// capture producer sleeps until descriptor destruction releases a slot.
	void RequestGpuVuInputRetirement(
		const VitaGpuVu::RawVifPayloadRef& blocked_generation);

	const u8* GetLocalMemoryForTrace(size_t* size);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
	bool CopyPrivilegedRegistersForValidation(u8* output, size_t size);
	u64 GetMtvuPath1CompletionDeferralsForValidation();
	bool IsMtvuPath1BufferWaiterArmedForValidation();
	bool ValidateMtvuPath1NoOutputCompletions();
#endif

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION) && VITASX2_PRODUCT_BOOT_VALIDATION
	struct CanonicalRingValidationResult
	{
		u32 canonical_bytes = 0;
		u32 packet_qwc = 0;
		u32 pixel_checks = 0;
		u32 address_checks = 0;
		u32 clut_cases = 0;
		u32 readback_checks = 0;
		u64 packet_hash = 0;
		u64 local_hash = 0;
		u64 reopened_hash = 0;
		bool reopened_clean = false;
	};

	bool ValidateCanonicalLocalMemoryRing(
		CanonicalRingValidationResult* result, Error* error);
#endif
}
