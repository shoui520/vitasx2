add_library(pcsx2core STATIC
	BuildVersion.cpp
	Cache.cpp
	CDVD/CDVD.cpp
	CDVD/CDVDcommon.cpp
	# Vita has no host optical drive. Do not link PCSX2's physical-drive
	# worker/cache or VitaCdvdDevice's former IOCtlSrc stubs. The ISO reader
	# and emulated CDVD command/timing owners below remain unchanged.
	CDVD/CDVDisoReader.cpp
	CDVD/FlatFileReader.cpp
	CDVD/InputIsoFile.cpp
	CDVD/IsoHasher.cpp
	CDVD/IsoReader.cpp
	CDVD/OutputIsoFile.cpp
	CDVD/Ps1CD.cpp
	CDVD/ThreadedFileReader.cpp
	COP0.cpp
	COP2.cpp
	Counters.cpp
	DebugTools/BiosDebugData.cpp
	DebugTools/Breakpoints.cpp
	DebugTools/DebugInterface.cpp
	DebugTools/DisR3000A.cpp
	DebugTools/DisR5900asm.cpp
	DebugTools/DisVU0Micro.cpp
	DebugTools/DisVU1Micro.cpp
	DebugTools/DisassemblyManager.cpp
	DebugTools/CoreEventTrace.cpp
	DebugTools/EeTrace.cpp
	DebugTools/GsTrace.cpp
	DebugTools/IopTrace.cpp
	DebugTools/IpuTrace.cpp
	DebugTools/MachineCheckpointTrace.cpp
	DebugTools/MemTrace.cpp
	DebugTools/SifTrace.cpp
	DebugTools/Spu2Trace.cpp
	DebugTools/VifTrace.cpp
	DebugTools/VuTrace.cpp
	DebugTools/ExpressionParser.cpp
	DebugTools/MIPSAnalyst.cpp
	DebugTools/MipsAssembler.cpp
	DebugTools/MipsAssemblerTables.cpp
	DebugTools/MipsStackWalk.cpp
	DebugTools/SymbolGuardian.cpp
	DebugTools/SymbolImporter.cpp
	Dmac.cpp
	Elfheader.cpp
	FPU.cpp
	FiFo.cpp
	GS.cpp
	GS/GSBlock.cpp
	GS/GSClut.cpp
	GS/GSDrawingContext.cpp
	GS/GSDrawingEnvironment.cpp
	GS/GSLocalMemory.cpp
	GS/GSLocalMemoryMultiISA.cpp
	GS/GSPerfMon.cpp
	GS/GSRingHeap.cpp
	GS/GSState.cpp
	GS/GSTables.cpp
	GS/GSUtil.cpp
	GS/GSVector.cpp
	GS/GSXXH.cpp
	GS/Renderers/Common/GSDevice.cpp
	GS/Renderers/Common/GSDirtyRect.cpp
	GS/Renderers/Common/GSFunctionMap.cpp
	GS/Renderers/Common/GSRenderer.cpp
	GS/Renderers/Common/GSTexture.cpp
	GS/Renderers/Common/GSVertexTrace.cpp
	GS/Renderers/Common/GSVertexTraceFMM.cpp
	GS/Renderers/HW/GSHwHack.cpp
	GS/Renderers/HW/GSRendererHW.cpp
	GS/Renderers/HW/GSRendererHWMultiISA.cpp
	GS/Renderers/HW/GSTextureCache.cpp
	GS/Renderers/SW/GSDrawScanline.cpp
	GS/Renderers/SW/GSRasterizer.cpp
	GS/Renderers/SW/GSRendererSW.cpp
	GS/Renderers/SW/GSTextureCacheSW.cpp
	Gif.cpp
	Gif_Logger.cpp
	Gif_Unit.cpp
	Host.cpp
	Hw.cpp
	HwRead.cpp
	HwWrite.cpp
	INISettingsInterface.cpp
	IPU/IPU.cpp
	IPU/IPU_Fifo.cpp
	IPU/IPU_MultiISA.cpp
	IPU/IPUdither.cpp
	IPU/IPUdma.cpp
	IPU/yuv2rgb.cpp
	Interpreter.cpp
	IopBios.cpp
	IopCounters.cpp
	IopDma.cpp
	IopGte.cpp
	IopHw.cpp
	IopIrq.cpp
	IopMem.cpp
	LayeredSettingsInterface.cpp
	MMI.cpp
	MTVU.cpp
	Mdec.cpp
	Memory.cpp
	MemoryMap.cpp
	Pcsx2Config.cpp
	PerformanceMetrics.cpp
	R3000A.cpp
	R3000AInterpreter.cpp
	R3000AOpcodeTables.cpp
	R5900.cpp
	R5900OpcodeImpl.cpp
	R5900OpcodeTables.cpp
	SaveStateRaw.cpp
	SIO/Pad/PadBase.cpp
	SIO/Pad/PadDualshock2.cpp
	SIO/Pad/PadNotConnected.cpp
	SIO/Memcard/MemoryCardFile.cpp
	SIO/Memcard/MemoryCardProtocol.cpp
	SIO/Multitap/MultitapProtocol.cpp
	SIO/Sio.cpp
	SIO/Sio0.cpp
	SIO/Sio2.cpp
	SPU2/ADSR.cpp
	SPU2/Debug.cpp
	SPU2/Dma.cpp
	SPU2/Mixer.cpp
	SPU2/ReadInput.cpp
	SPU2/RegTable.cpp
	SPU2/Reverb.cpp
	SPU2/ReverbResample.cpp
	SPU2/Wavedump_wav.cpp
	SPU2/spu2.cpp
	SPU2/spu2freeze.cpp
	SPU2/spu2sys.cpp
	SPR.cpp
	ShiftJisToUnicode.cpp
	Sif.cpp
	Sif0.cpp
	Sif1.cpp
	SourceLog.cpp
	StateWrapper.cpp
	USB/USB.cpp
	USB/qemu-usb/bus.cpp
	USB/qemu-usb/core.cpp
	USB/qemu-usb/desc.cpp
	USB/qemu-usb/usb-ohci.cpp
	VU0.cpp
	VU0micro.cpp
	VU0microInterp.cpp
	VU1micro.cpp
	VU1microInterp.cpp
	VUflags.cpp
	VUmicro.cpp
	VUmicroMem.cpp
	VUops.cpp
	Vif.cpp
	Vif0_Dma.cpp
	Vif1_Dma.cpp
	Vif1_MFIFO.cpp
	Vif_Codes.cpp
	Vif_Transfer.cpp
	Vif_Unpack.cpp
	ps2/BiosTools.cpp
	ps2/Iop/IopHwRead.cpp
	ps2/Iop/IopHwWrite.cpp
	ps2/Iop/PsxBios.cpp
	ps2/pgif.cpp
	sif2.cpp
	vita/VitaAudioStream.cpp
	vita/A32Emitter.cpp
	vita/VitaCaptureDisabled.cpp
	vita/VitaCpuProviders.cpp
	vita/VitaDisabledDevices.cpp
	vita/VitaEeBlockCompiler.cpp
	vita/VitaEeExecutor.cpp
	vita/VitaEeRegionA32.cpp
	vita/VitaEeRegionAllocatedA32.cpp
	vita/VitaEeRegionExecutionPlan.cpp
	vita/VitaEeRegionAllocation.cpp
	vita/VitaEeRegionIR.cpp
	vita/VitaEeRegionMemory.cpp
	vita/VitaEeSemanticIsland.cpp
	vita/VitaEeAdpcmDecode.cpp
	vita/VitaEeSemanticKernel.cpp
	vita/VitaEeRegionRuntime.cpp
	vita/VitaGpuVuProgram.cpp
	vita/VitaGpuVuArchitectureLedger.cpp
	vita/VitaGpuVuMicroProgram.cpp
	vita/VitaGpuVuCommandEpoch.cpp
	vita/VitaGpuVuUniversalEpoch.cpp
	vita/VitaGpuVuGeneratedUniversal.cpp
	vita/VitaGpuVuGxpResource.cpp
	vita/VitaGpuVuHealthJournal.cpp
	vita/VitaGpuVuOpportunityCensus.cpp
	vita/VitaGpuVuVifInput.cpp
	vita/VitaGpuVuLoopKernel.cpp
	vita/VitaGpuVuInvocationPlan.cpp
	vita/VitaGpuVuCgGenerator.cpp
	vita/VitaGpuVuDirectProgram.cpp
	vita/VitaGpuVuDraw.cpp
	vita/VitaGpuVuEntrySlice.cpp
	vita/VitaGpuVuGifContract.cpp
	vita/VitaGpuVuShaderCompiler.cpp
	vita/VitaGpuVuProgramRegistry.cpp
	vita/VitaIopBlockCompiler.cpp
	vita/GSDeviceGXM.cpp
	vita/VitaGxmArena.cpp
	vita/VitaGxmGsState.cpp
	vita/VitaGxmDisplay.cpp
	vita/VitaGxmMemory.cpp
	vita/VitaGxmRenderStore.cpp
	vita/VitaGxmTexture.cpp
	vita/VitaGsDrawTrace.cpp
	vita/VitaGsMailbox.cpp
	vita/VitaGsDiagnosticsDisabled.cpp
	vita/VitaGsRuntime.cpp
	vita/VitaGsTextureReplacements.cpp
	vita/VitaGsMemory.cpp
	vita/VitaGsMemory.h
	vita/VitaHostSurface.cpp
	vita/VitaInputManager.cpp
	vita/VitaPad.cpp
	vita/VitaPerformanceTelemetry.cpp
	vita/VitaSaveState.cpp
	vita/VitaUsbDeviceRegistry.cpp
	vita/VitaVifEpochCensus.cpp
	vita/VitaVifInterpreter.cpp
	vita/VitaVmState.cpp
	vita/VitaVuBlockCompiler.cpp
	vtlb.cpp
)

# The private EE-to-IOP dispatcher entry skips the compiler's forced first
# PUSH after verifying its exact A32 opcode at runtime. Keep this translation
# unit non-PIE so no GOT setup can legally precede that save.
set_source_files_properties(vita/VitaIopBlockCompiler.cpp PROPERTIES
	COMPILE_OPTIONS "-fno-pie;-marm"
)
option(VITASX2_GXM_GS_BLEND
	"Lower GS blend equations directly to GXM (renderer migration candidate)." OFF)
option(VITASX2_GXM_GS_BLEND_VALIDATION
	"Validate direct GS blending on private GXM surfaces." OFF)
if(VITASX2_GXM_GS_BLEND)
	set_property(SOURCE GS/Renderers/HW/GSRendererHW.cpp vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_GS_BLEND=1)
endif()
if(VITASX2_GXM_GS_BLEND_VALIDATION)
	set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_GS_BLEND_VALIDATION=1)
endif()
option(VITASX2_GXM_FUSE_ATTACHMENT_CLEARS
	"Materialize lazy clears in the requested GXM attachment scene."
	OFF)
option(VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION
	"Compare separate and fused lazy clears on private startup surfaces."
	OFF)
foreach(option_name VITASX2_GXM_FUSE_ATTACHMENT_CLEARS VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION)
	if(${option_name})
		set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
			COMPILE_DEFINITIONS ${option_name}=1)
	endif()
endforeach()
option(VITASX2_GXM_TFX_ALPHA_VALIDATION
	"Compare opaque alpha-test specializations on private startup surfaces."
	OFF)
if(VITASX2_GXM_TFX_ALPHA_VALIDATION)
	set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_TFX_ALPHA_VALIDATION=1)
endif()
option(VITASX2_GXM_SCISSOR_VALIDATION
	"Validate persistent scissor mask/depth/color storage on private startup surfaces."
	OFF)
if(VITASX2_GXM_SCISSOR_VALIDATION)
	set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_SCISSOR_VALIDATION=1)
endif()
option(VITASX2_GXM_SCISSOR_REBUILD_CONTROL
	"Force mask reconstruction at every depth scene for matched Vita controls."
	OFF)
if(VITASX2_GXM_SCISSOR_REBUILD_CONTROL)
	set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_SCISSOR_REBUILD_CONTROL=1)
endif()
option(VITASX2_GXM_PERSISTENT_RENDER_STORE
	"Use the experimental shared GXM render-store residency planner."
	OFF)
if(VITASX2_GXM_PERSISTENT_RENDER_STORE)
	set_property(SOURCE vita/GSDeviceGXM.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_GXM_PERSISTENT_RENDER_STORE=1)
endif()
if(VITASX2_IOP_SCHEDULER_PRE_EVENT_WAIT_ADVANCE_CONTROL)
	set_property(SOURCE vita/VitaIopBlockCompiler.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS
			VITASX2_IOP_SCHEDULER_PRE_EVENT_WAIT_ADVANCE_CONTROL=1
	)
endif()
if(VITASX2_SILENT_HSYNC_FOLD_CONTROL)
	set_property(SOURCE Counters.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS VITASX2_SILENT_HSYNC_FOLD_CONTROL=1
	)
endif()
if(VITASX2_EE_INTC_VBLANK_RAM_WAIT_CONTROL)
	set_property(SOURCE vita/VitaEeBlockCompiler.cpp APPEND PROPERTY
		COMPILE_DEFINITIONS
			VITASX2_EE_INTC_VBLANK_RAM_WAIT_CONTROL=1
	)
endif()

# The VU1 generated-code ABI owns Q4-Q7 for VF/ACC residency.  Its outer
# execution wrapper preserves D8-D15 once for AAPCS; prevent the C++ control
# code in the same translation unit from allocating those registers while the
# private generated chain is active.
set_source_files_properties(vita/VitaVuBlockCompiler.cpp PROPERTIES
	COMPILE_OPTIONS "-ffixed-d8;-ffixed-d9;-ffixed-d10;-ffixed-d11;-ffixed-d12;-ffixed-d13;-ffixed-d14;-ffixed-d15"
)

# VU multiply/add instructions round after each architectural operation.
# Contracting the interpreter's separate expressions changes PS2-visible bits
# and makes trace output compiler-dependent, so keep contraction disabled only
# for the translation unit which owns those expressions.
set_source_files_properties(VUops.cpp PROPERTIES
	COMPILE_OPTIONS "-ffp-contract=off"
)

set(VITASX2_GIT_TAG "vitasx2-ng")
set(VITASX2_GIT_REV "vitasx2-ng-pcsx2-fe3ce7f57")
set(VITASX2_GIT_HASH "fe3ce7f578e2dd39418ef358d84804d290e91bc1")
set(VITASX2_GIT_DATE "2026-07-01")
configure_file(vita/svnrev.h.in "${CMAKE_CURRENT_BINARY_DIR}/svnrev.h" @ONLY)

target_include_directories(pcsx2core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"
	"${CMAKE_SOURCE_DIR}/3rdparty/include"
)

target_compile_definitions(pcsx2core PUBLIC
	PCSX2_CORE=1
	VITASX2_VITA=1
)

target_compile_options(pcsx2core PRIVATE
	-Wno-invalid-offsetof
	-Wno-missing-braces
	-Wno-multichar
	-Wno-parentheses
	-Wno-unknown-pragmas
)

target_link_libraries(pcsx2core PUBLIC
	common
	fmt::fmt
	fast_float
	simpleini
	rapidjson
	xxhash
	ccc
	demanglegnu
	SceCtrl_stub
	SceAudio_stub
	SceAppMgr_stub
	SceDisplay_stub
	SceGxm_stub
	SceKernelModulemgr_stub
	SceShaccCg_stub
	SceShaccCgExt
	SceSysmodule_stub
	SceNet_stub
	SceVshBridge_stub
	taihen_stub
	kubridge_stub_weak
)

# Sony's api_libgxm/basic sample owns the shader pipeline used here: compile
# Cg to GXP with psp2cgc, then embed the program with psp2bin at four-byte
# alignment. VitaSDK deliberately does not ship Sony's shader compiler.
set(VITASX2_PSP2_SDK_ROOT
	"$ENV{VITASX2_PSP2_SDK_ROOT}"
	CACHE PATH "Root of the official Sony PSP2 SDK used for GXM shader compilation")
set(VITASX2_PSP2CGC_3570_PATCH
	"$ENV{VITASX2_PSP2CGC}")
set(VITASX2_PSP2CGC_DEFAULT
	"${VITASX2_PSP2_SDK_ROOT}/host_tools/bin/psp2cgc.exe")
if(EXISTS "${VITASX2_PSP2CGC_3570_PATCH}")
	# Sony's 3.570.021 compiler patch is build 13284, superseding the base
	# 3.5.0 build 13276. Both produce the same measured FMAC resource shape;
	# prefer the maintenance build without making it a source-support gate.
	set(VITASX2_PSP2CGC_DEFAULT "${VITASX2_PSP2CGC_3570_PATCH}")
endif()
set(VITASX2_PSP2CGC "${VITASX2_PSP2CGC_DEFAULT}"
	CACHE FILEPATH "Official Sony PSP2 Cg compiler executable")
set(VITASX2_PSP2BIN
	"${VITASX2_PSP2_SDK_ROOT}/host_tools/build/bin/psp2bin.exe")
set(VITASX2_GXM_SHADER_OBJECTS)
set(VITASX2_GXM_SHADER_ASSETS)
set(VITASX2_GPU_VU_UNIVERSAL_CG_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vu_universal_v.cg")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${VITASX2_GPU_VU_UNIVERSAL_CG_PATH}")
file(READ "${VITASX2_GPU_VU_UNIVERSAL_CG_PATH}"
	VITASX2_GPU_VU_UNIVERSAL_CG_SOURCE)
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/vita/VitaGpuVuUniversalCgSource.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/VitaGpuVuUniversalCgSource.h" @ONLY)
set(VITASX2_GPU_VU_STRUCTURED_PREFLIGHT_CG_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vu_structured_preflight_v.cg")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${VITASX2_GPU_VU_STRUCTURED_PREFLIGHT_CG_PATH}")
file(READ "${VITASX2_GPU_VU_STRUCTURED_PREFLIGHT_CG_PATH}"
	VITASX2_GPU_VU_STRUCTURED_PREFLIGHT_CG_SOURCE)
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/vita/VitaGpuVuStructuredPreflightCgSource.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/VitaGpuVuStructuredPreflightCgSource.h" @ONLY)
if(NOT EXISTS "${VITASX2_PSP2CGC}" OR NOT EXISTS "${VITASX2_PSP2BIN}")
	# Keep non-core utilities configurable without the proprietary shader tools.
	# The dependency makes every pcsx2core-linked build fail at build time with a
	# targeted diagnostic instead of silently producing a renderer without GXP.
	add_custom_target(vitasx2-gxm-shaders
		COMMAND "${CMAKE_COMMAND}" -E echo
			"The official PSP2 SDK psp2cgc/psp2bin tools are required for pcsx2core; set VITASX2_PSP2_SDK_ROOT."
		COMMAND "${CMAKE_COMMAND}" -E false
		VERBATIM)
else()
	function(vitasx2_add_gxm_shader shader_name profile)
		set(shader_source
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/${shader_name}.cg")
		set(shader_gxp "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.gxp")
		set(shader_object "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}_gxp.obj")
		add_custom_command(
			OUTPUT "${shader_object}"
			BYPRODUCTS "${shader_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile "${profile}"
				"${shader_source}" -o "${shader_gxp}"
			COMMAND "${VITASX2_PSP2BIN}" "${shader_gxp}"
				-b2e "PSP2,_binary_${shader_name}_gxp_start,_binary_${shader_name}_gxp_size,4,_binary_${shader_name}_gxp_end"
				-o "${shader_object}"
			DEPENDS "${shader_source}" ${ARGN}
			VERBATIM)
		set_source_files_properties("${shader_object}" PROPERTIES
			GENERATED TRUE EXTERNAL_OBJECT TRUE)
		set(VITASX2_GXM_SHADER_OBJECTS
			${VITASX2_GXM_SHADER_OBJECTS} "${shader_object}" PARENT_SCOPE)
	endfunction()

	# Validation-only programs read through an explicitly bounded GXM state
	# owner must not also remain duplicated in the SELF's writable data. Compile
	# the GXP as a sidecar and retain it only through shader unregister.
	function(vitasx2_add_gxm_shader_asset shader_name profile)
		set(shader_source
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/${shader_name}.cg")
		set(shader_gxp "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.gxp")
		add_custom_command(
			OUTPUT "${shader_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile "${profile}"
				-nofastmath -bestprecision
				-DVITASX2_GPU_VU_DUAL_PRIVATE_GENERATION=1
				"${shader_source}" -o "${shader_gxp}"
			DEPENDS "${shader_source}" ${ARGN}
			VERBATIM)
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} "${shader_gxp}" PARENT_SCOPE)
	endfunction()

	# A playable-profile arithmetic asset still preserves operation order and
	# native binary32 precision, but intentionally accepts the attested SGX final
	# rounding result instead of the integer nearest-even helpers compiled into
	# the exact sibling GXP.
	function(vitasx2_add_gxm_native_shader_asset shader_name profile)
		set(shader_source
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/${shader_name}.cg")
		set(shader_gxp "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.gxp")
		add_custom_command(
			OUTPUT "${shader_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile "${profile}"
				-O3 -nofastmath -nofastprecision
				-DVITASX2_GPU_VU_DUAL_PRIVATE_GENERATION=1
				"${shader_source}" -o "${shader_gxp}"
			DEPENDS "${shader_source}" ${ARGN}
			VERBATIM)
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} "${shader_gxp}" PARENT_SCOPE)
	endfunction()

	# VIF UNPACK has two title-neutral fixed tiers from the same PCSX2-owned
	# source. The independent root handles non-row-recursive, non-skipping
	# commands without a 128-entry indexable local journal. Both roots mirror
	# sparse writes into the two private banks, which were initialized
	# byte-identically before submission, so an UNPACK job need not copy 16 KiB
	# or alternate banks. The serial sibling retains complete supported-command
	# compatibility when vector order is architecturally observable.
	function(vitasx2_add_gpu_vu_vif_unpack_assets)
		set(shader_source
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vif_unpack_v.cg")
		set(serial_gxp
			"${CMAKE_CURRENT_BINARY_DIR}/vitasx2_gpu_vif_unpack_v.gxp")
		set(independent_gxp
			"${CMAKE_CURRENT_BINARY_DIR}/vitasx2_gpu_vif_unpack_independent_v.gxp")
		add_custom_command(
			OUTPUT "${serial_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile sce_vp_psp2
				-nofastmath -bestprecision
				-DVITASX2_GPU_VU_DUAL_PRIVATE_GENERATION=1
				-DVITASX2_GPU_VU_MIRRORED_PRIVATE_GENERATION=1
				"${shader_source}" -o "${serial_gxp}"
			DEPENDS "${shader_source}"
			VERBATIM)
		add_custom_command(
			OUTPUT "${independent_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile sce_vp_psp2
				-O3 -nofastmath -bestprecision
				-DVITASX2_GPU_VU_DUAL_PRIVATE_GENERATION=1
				-DVITASX2_GPU_VU_MIRRORED_PRIVATE_GENERATION=1
				-DVITASX2_GPU_VU_INDEPENDENT_UNPACK=1
				"${shader_source}" -o "${independent_gxp}"
			DEPENDS "${shader_source}"
			VERBATIM)
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} "${serial_gxp}"
			"${independent_gxp}" PARENT_SCOPE)
	endfunction()

	# The scheduled universal continuation is a fixed product provider, not a
	# generated-program cache entry. Compile the same semantic core offline with
	# the command scanner and unscheduled hazard machinery removed. Runtime
	# ShaccCg 3.0 cannot compile this complete core within its bounded arena,
	# while Sony's offline compiler accepts it comfortably inside the sidecar
	# reservation.
	function(vitasx2_add_gpu_vu_compact_asset)
		set(shader_source
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vu_universal_v.cg")
		set(shader_gxp
			"${CMAKE_CURRENT_BINARY_DIR}/vitasx2_gpu_vu_compact_v.gxp")
		add_custom_command(
			OUTPUT "${shader_gxp}"
			COMMAND "${VITASX2_PSP2CGC}" --profile sce_vp_psp2
				-O1 -nofastmath -bestprecision
				-DVITASX2_GPU_VU_COMPACT_CONTINUATION=1
				-DVITASX2_GPU_VU_DUAL_PRIVATE_GENERATION=1
				-DVITASX2_UNIVERSAL_PAIR_SLICE=256
				"${shader_source}" -o "${shader_gxp}"
			DEPENDS "${shader_source}"
			VERBATIM)
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} "${shader_gxp}" PARENT_SCOPE)
	endfunction()

	function(vitasx2_add_gpu_vu_structured_preflight_assets)
		set(preflight_assets)
		foreach(stage build reduce finalize)
			set(shader_source
				"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vu_structured_preflight_${stage}_v.cg")
			set(shader_gxp
				"${CMAKE_CURRENT_BINARY_DIR}/vitasx2_gpu_vu_structured_preflight_${stage}_v.gxp")
			add_custom_command(
				OUTPUT "${shader_gxp}"
				COMMAND "${VITASX2_PSP2CGC}" --profile sce_vp_psp2
					-O1 -nofastmath -bestprecision
					"${shader_source}" -o "${shader_gxp}"
				DEPENDS "${shader_source}"
				VERBATIM)
			list(APPEND preflight_assets "${shader_gxp}")
		endforeach()
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} ${preflight_assets} PARENT_SCOPE)
	endfunction()

	# Architectural loop control and state recurrence are interpreted from
	# bounded PairPlan-derived bytecode by two dependency-ordered offline GXPs.
	# Runtime ShaccCg is deliberately absent from these stages; physical r31/r32
	# showed repeatable compiler faults even for one isolated recurrence vector.
	function(vitasx2_add_gpu_vu_structured_fixed_assets)
		set(structured_fixed_assets)
		foreach(stage control state)
			set(shader_source
				"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_gpu_vu_structured_${stage}_v.cg")
			set(shader_gxp
				"${CMAKE_CURRENT_BINARY_DIR}/vitasx2_gpu_vu_structured_${stage}_v.gxp")
			add_custom_command(
				OUTPUT "${shader_gxp}"
				COMMAND "${VITASX2_PSP2CGC}" --profile sce_vp_psp2
					-O1 -nofastmath -bestprecision
					"${shader_source}" -o "${shader_gxp}"
				DEPENDS "${shader_source}"
				VERBATIM)
			list(APPEND structured_fixed_assets "${shader_gxp}")
		endforeach()
		set(VITASX2_GXM_SHADER_ASSETS
			${VITASX2_GXM_SHADER_ASSETS} ${structured_fixed_assets} PARENT_SCOPE)
	endfunction()

	vitasx2_add_gxm_shader(vitasx2_color_v sce_vp_psp2)
	vitasx2_add_gxm_shader(vitasx2_color_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_present_v sce_vp_psp2)
	vitasx2_add_gxm_shader(vitasx2_present_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_merge_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_copy_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_rta_correction_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_rta_decorrection_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_mad_buffer_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_mad_reconstruct_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_tfx_v sce_vp_psp2)
	vitasx2_add_gxm_shader(vitasx2_tfx_f sce_fp_psp2)
	foreach(variant "" _zfloor _no_atst _no_atst_zfloor
		_source _source_zfloor _source_no_atst _source_no_atst_zfloor)
		vitasx2_add_gxm_shader(vitasx2_tfx_gs_blend${variant}_f sce_fp_psp2
			"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	endforeach()
	vitasx2_add_gxm_shader(vitasx2_tfx_gs_blend_direct_modulate_stq_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_gs_blend_direct_modulate_stq_zfloor_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_manual_lod_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_manual_lod_zfloor_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_zfloor_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_psm16_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_psm16_zfloor_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_zfloor_source_direct_decal_af_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_fast_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_fast_no_atst_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_uv_no_fog_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_uv_no_fog_fast_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_uv_no_fog_zfloor_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_region_repeat_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_region_repeat_fast_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_untextured_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_source_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_programmable_add_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_programmable_add_direct_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_programmable_over_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_source_direct_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_source_direct_modulate_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_source_direct_modulate_af_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	vitasx2_add_gxm_shader(vitasx2_tfx_source_untextured_f sce_fp_psp2
		"${CMAKE_CURRENT_SOURCE_DIR}/vita/gxm/shaders/vitasx2_tfx_f.cg")
	if(VITASX2_GPU_VU_UNIVERSAL_VALIDATION)
		vitasx2_add_gxm_shader_asset(vitasx2_gpu_vu_universal_v sce_vp_psp2)
		vitasx2_add_gpu_vu_vif_unpack_assets()
		vitasx2_add_gxm_shader_asset(vitasx2_gpu_vu_structured_qp_numeric_v sce_vp_psp2)
		vitasx2_add_gxm_shader_asset(vitasx2_gpu_vu_structured_fmac_numeric_v sce_vp_psp2)
		vitasx2_add_gxm_native_shader_asset(vitasx2_gpu_vu_structured_fmac_native_v sce_vp_psp2)
		vitasx2_add_gxm_shader_asset(vitasx2_gpu_vu_path1_commit_v sce_vp_psp2)
		vitasx2_add_gpu_vu_compact_asset()
		vitasx2_add_gpu_vu_structured_fixed_assets()
		vitasx2_add_gpu_vu_structured_preflight_assets()
		vitasx2_add_gxm_shader(vitasx2_gpu_vu_universal_f sce_fp_psp2)
	endif()
	# INTERFACE object sources are direct final-link inputs, but CMake does not
	# retain their generation edge. Give the outputs an owner and pass them to
	# final executables because GNU ar does not index psp2bin's slot-zero symbol.
	add_custom_target(vitasx2-gxm-shaders DEPENDS
		${VITASX2_GXM_SHADER_OBJECTS} ${VITASX2_GXM_SHADER_ASSETS})
	target_sources(pcsx2core INTERFACE ${VITASX2_GXM_SHADER_OBJECTS})
endif()
add_dependencies(pcsx2core vitasx2-gxm-shaders)
