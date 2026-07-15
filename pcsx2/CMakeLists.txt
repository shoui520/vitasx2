add_library(pcsx2core STATIC
	BuildVersion.cpp
	Cache.cpp
	CDVD/CDVD.cpp
	CDVD/CDVDcommon.cpp
	CDVD/CDVDdiscReader.cpp
	CDVD/CDVDdiscThread.cpp
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
	GS/GSState.cpp
	GS/GSTables.cpp
	GS/GSUtil.cpp
	GS/GSVector.cpp
	GS/GSXXH.cpp
	GS/Renderers/Common/GSVertexTrace.cpp
	GS/Renderers/Common/GSVertexTraceFMM.cpp
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
	vita/VitaCdvdDevice.cpp
	vita/VitaCpuProviders.cpp
	vita/VitaDisabledDevices.cpp
	vita/VitaEeBlockCompiler.cpp
	vita/VitaEeExecutor.cpp
	vita/VitaIopBlockCompiler.cpp
	vita/VitaGxmGsState.cpp
	vita/VitaGxmDisplay.cpp
	vita/VitaGxmMemory.cpp
	vita/VitaGsMailbox.cpp
	vita/VitaGsDiagnosticsDisabled.cpp
	vita/VitaGsMemory.cpp
	vita/VitaHostSurface.cpp
	vita/VitaInputManager.cpp
	vita/VitaPad.cpp
	vita/VitaSaveState.cpp
	vita/VitaUsbDeviceRegistry.cpp
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
	SceDisplay_stub
	SceGxm_stub
)

# Sony's api_libgxm/basic sample owns the shader pipeline used here: compile
# Cg to GXP with psp2cgc, then embed the program with psp2bin at four-byte
# alignment. VitaSDK deliberately does not ship Sony's shader compiler.
set(VITASX2_PSP2_SDK_ROOT
	"$ENV{VITASX2_PSP2_SDK_ROOT}"
	CACHE PATH "Root of the official Sony PSP2 SDK used for GXM shader compilation")
set(VITASX2_PSP2CGC
	"${VITASX2_PSP2_SDK_ROOT}/host_tools/bin/psp2cgc.exe")
set(VITASX2_PSP2BIN
	"${VITASX2_PSP2_SDK_ROOT}/host_tools/build/bin/psp2bin.exe")
set(VITASX2_GXM_SHADER_OBJECTS)
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
			DEPENDS "${shader_source}"
			VERBATIM)
		set_source_files_properties("${shader_object}" PROPERTIES
			GENERATED TRUE EXTERNAL_OBJECT TRUE)
		set(VITASX2_GXM_SHADER_OBJECTS
			${VITASX2_GXM_SHADER_OBJECTS} "${shader_object}" PARENT_SCOPE)
	endfunction()

	vitasx2_add_gxm_shader(vitasx2_color_v sce_vp_psp2)
	vitasx2_add_gxm_shader(vitasx2_color_f sce_fp_psp2)
	vitasx2_add_gxm_shader(vitasx2_present_v sce_vp_psp2)
	vitasx2_add_gxm_shader(vitasx2_present_f sce_fp_psp2)
	# INTERFACE object sources are direct final-link inputs, but CMake does not
	# retain their generation edge. Give the outputs an owner and pass them to
	# final executables because GNU ar does not index psp2bin's slot-zero symbol.
	add_custom_target(vitasx2-gxm-shaders DEPENDS ${VITASX2_GXM_SHADER_OBJECTS})
	target_sources(pcsx2core INTERFACE ${VITASX2_GXM_SHADER_OBJECTS})
endif()
add_dependencies(pcsx2core vitasx2-gxm-shaders)
