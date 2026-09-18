# Vita-native layer: host runtime, diagnostics, GXM backend, A32 emitters.

set(VITASX2_TITLEID "VSX200001")
set(VITASX2_CORE_VALIDATION_TITLEID "VSX200002")
set(VITASX2_BIOS_VALIDATION_TITLEID "VSX200003")
set(VITASX2_BIOS_EE_TRACE_TITLEID "VSX200004")
set(VITASX2_BIOS_IOP_TRACE_TITLEID "VSX200005")
set(VITASX2_CUBE_EE_TRACE_TITLEID "VSX200006")
set(VITASX2_SIMPLE2000_EE_TRACE_TITLEID "VSX200007")
set(VITASX2_A32_EMITTER_VALIDATION_TITLEID "VSX200008")
set(VITASX2_A32_PROVIDER_VALIDATION_TITLEID "VSX200009")
set(VITASX2_BIOS_EE_A32_TRACE_TITLEID "VSX200010")
set(VITASX2_CUBE_EE_A32_TRACE_TITLEID "VSX200011")

set(VITASX2_CUBE_EE_TRACE_RECORDS "4096" CACHE STRING "Cube EE trace records to write.")
set(VITASX2_CUBE_EE_TRACE_SKIP_RECORDS "0" CACHE STRING "Cube EE trace records to skip before writing.")
option(VITASX2_CUBE_EE_TRACE_FROM_BOOT "Start the cube EE trace at BIOS boot instead of ELF entry." OFF)

set(VITASX2_SIMPLE2000_EE_TRACE_RECORDS "4096" CACHE STRING "Simple 2000 EE trace records to write.")
set(VITASX2_SIMPLE2000_EE_TRACE_SKIP_RECORDS "0" CACHE STRING "Simple 2000 EE trace records to skip before writing.")
option(VITASX2_SIMPLE2000_EE_TRACE_FROM_BOOT "Start the Simple 2000 EE trace at BIOS boot instead of ELF entry." OFF)

# --- normal Vita product -----------------------------------------------------
add_executable(vitasx2
	product/VitaSx2Main.cpp
)
target_link_libraries(vitasx2
	pcsx2core
	LZMA::LZMA
)

# VitaSX2's persistent emulator/JIT/GXM owners require the supported extended
# application-memory budget. This emits PARAM.SFO ATTRIBUTE2=12 (365 MiB) for
# the sole VSX200001 product container; it is not an unsafe runtime allocation
# workaround and leaves CDRAM ownership unchanged.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_self(vitasx2.self vitasx2 UNSAFE)
vita_create_vpk(vitasx2.vpk ${VITASX2_TITLEID} vitasx2.self
	VERSION "00.01"
	NAME "VitaSX2"
)

# Explicit compiler-service lifecycle regression, never part of a product
# build. Compile the actual owner against the real SDK/runtime; only the GS
# wakeup sink is replaced (no renderer or guest execution in this executable).
add_executable(vitasx2-shader-compiler-service-validation EXCLUDE_FROM_ALL
	validation/ShaderCompilerServiceMain.cpp
	../pcsx2/vita/VitaGpuVuShaderCompiler.cpp
	../pcsx2/vita/VitaGpuVuGxpResource.cpp
)
target_include_directories(vitasx2-shader-compiler-service-validation PRIVATE
	"${CMAKE_SOURCE_DIR}/pcsx2")
target_compile_definitions(vitasx2-shader-compiler-service-validation PRIVATE
	VITASX2_GPU_VU_COMPILER_CACHE_DIRECTORY="ux0:data/vitasx2/compiler-service-validation/cache"
	VITASX2_GPU_VU_COMPILER_AUDIT_DIRECTORY="ux0:data/vitasx2/compiler-service-validation/audit"
)
target_link_libraries(vitasx2-shader-compiler-service-validation PRIVATE
	common SceKernelModulemgr_stub SceShaccCg_stub SceShaccCgExt
	SceSysmodule_stub taihen_stub)
vita_create_self(vitasx2-shader-compiler-service-validation.self
	vitasx2-shader-compiler-service-validation UNSAFE)
set_target_properties(
	vitasx2-shader-compiler-service-validation-velf
	vitasx2-shader-compiler-service-validation.self-self
	PROPERTIES EXCLUDE_FROM_ALL TRUE)

# --- finite Venezia/NEON hardware characterization --------------------------
# This target deliberately reuses the normal VSX200001 container. It is absent
# from product builds and embeds its MeP payload so the Vita run has no mutable
# sidecar files.
if(VITASX2_VENEZIA_BENCHMARK)
	set(VITASX2_VENEZIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/venezia")
	set(VITASX2_MEP_GCC "${VITASX2_VENEZIA_SDK_ROOT}/bin/mep-elf-gcc")
	set(VITASX2_MEP_INCLUDE "${VITASX2_VENEZIA_SDK_ROOT}/mep-elf/include")
	set(VITASX2_MEP_LIBRARY "${VITASX2_VENEZIA_SDK_ROOT}/mep-elf/lib")
	set(VITASX2_MEP_FLOAT_SOURCE
		"${VITASX2_VVNZRUNNER_SOURCE_ROOT}/samples_new/vnz_sample_hello_draw/src/float.c")
	if(NOT EXISTS "${VITASX2_MEP_GCC}")
		message(FATAL_ERROR
			"Venezia benchmark needs ${VITASX2_MEP_GCC}; set VITASX2_VENEZIA_SDK_ROOT.")
	endif()
	if(NOT EXISTS "${VITASX2_MEP_INCLUDE}/vnz/vthread.h" OR
		NOT EXISTS "${VITASX2_MEP_LIBRARY}/libvkernel.a")
		message(FATAL_ERROR
			"${VITASX2_VENEZIA_SDK_ROOT} lacks the Venezia headers/libvkernel target library.")
	endif()
	if(NOT EXISTS "${VITASX2_MEP_FLOAT_SOURCE}")
		message(FATAL_ERROR
			"Venezia benchmark needs VVNZRunner's float.c; set VITASX2_VVNZRUNNER_SOURCE_ROOT.")
	endif()

	set(VITASX2_MEP_ELF "${CMAKE_CURRENT_BINARY_DIR}/mep_code.elf")
	set(VITASX2_MEP_BLOB "${CMAKE_CURRENT_BINARY_DIR}/mep_code.o")
	set(VITASX2_MEP_SOURCES
		"${VITASX2_VENEZIA_DIR}/mep/crt0.c"
		"${VITASX2_VENEZIA_DIR}/mep/main.c"
		"${VITASX2_MEP_FLOAT_SOURCE}"
	)
	set(VITASX2_MEP_MACHINE_FLAGS
		-mtiny=0 -mabsdiff -maverage -mbitops -mclip -mdiv -mleadz
		-mminmax -mmult -mrepeat -msatur -mvl64 -mvliw -mcop64 -mivc2
		-D__MEP_CONFIG_CP_DATA_BUS_WIDTH=64
	)
	add_custom_command(
		OUTPUT "${VITASX2_MEP_ELF}"
		COMMAND "${VITASX2_MEP_GCC}"
			${VITASX2_MEP_SOURCES}
			-I${VITASX2_VENEZIA_DIR}
			-I${VITASX2_MEP_INCLUDE}
			-I${VITASX2_MEP_INCLUDE}/libk
			-L${VITASX2_MEP_LIBRARY}
			-nostdlib -nodefaultlibs -nostartfiles -std=c99 -O3
			-ffunction-sections -fdata-sections -Wl,-q
			${VITASX2_MEP_MACHINE_FLAGS}
			-lvkernel -lgcc -o "${VITASX2_MEP_ELF}"
		DEPENDS ${VITASX2_MEP_SOURCES}
			"${VITASX2_VENEZIA_DIR}/VeneziaBenchmarkProtocol.h"
		VERBATIM
		COMMENT "Building the Venezia MeP-c5/IVC2 benchmark payload"
	)
	add_custom_command(
		OUTPUT "${VITASX2_MEP_BLOB}"
		COMMAND "${CMAKE_OBJCOPY}" -I binary -O elf32-littlearm -B arm
			--rename-section .data=.rodata,alloc,load,readonly,data,contents
			mep_code.elf mep_code.o
		DEPENDS "${VITASX2_MEP_ELF}"
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
		VERBATIM
		COMMENT "Embedding the Venezia MeP executable in the Vita SELF"
	)
	set_source_files_properties("${VITASX2_MEP_BLOB}" PROPERTIES
		GENERATED TRUE
		EXTERNAL_OBJECT TRUE
	)

	set(VITASX2_VVNZ_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/vvnzrunner-stubs")
	file(MAKE_DIRECTORY "${VITASX2_VVNZ_STUB_DIR}")
	execute_process(
		COMMAND "${VITASDK}/bin/vita-libs-gen" -c
			"${VITASX2_VENEZIA_DIR}/VVNZRunner.yml" "${VITASX2_VVNZ_STUB_DIR}"
		RESULT_VARIABLE VITASX2_VVNZ_STUB_RESULT
	)
	if(NOT VITASX2_VVNZ_STUB_RESULT EQUAL 0)
		message(FATAL_ERROR "vita-libs-gen failed for the VVNZRunner import library.")
	endif()
	add_subdirectory("${VITASX2_VVNZ_STUB_DIR}"
		"${CMAKE_CURRENT_BINARY_DIR}/vvnzrunner-stub-build")

	add_executable(vitasx2-venezia-benchmark
		venezia/VeneziaBenchmarkMain.cpp
		"${VITASX2_MEP_BLOB}"
	)
	target_link_libraries(vitasx2-venezia-benchmark
		VVNZRunner_stub
		SceLibKernel_stub
		SceIofilemgr_stub
		SceSysmem_stub
		ScePower_stub
	)
	vita_create_self(vitasx2-venezia-benchmark.self vitasx2-venezia-benchmark UNSAFE)
	vita_create_vpk(vitasx2-venezia-benchmark.vpk ${VITASX2_TITLEID}
		vitasx2-venezia-benchmark.self
		VERSION "00.01"
		NAME "VitaSX2 Venezia Benchmark"
	)
endif()

# --- all-native portable replay validation ----------------------------------
# This target is deliberately absent from ordinary builds. It reuses the one
# installed VSX200001 container and enables only the bounded PCSX2 trace hooks
# needed to compare a real-hardware continuation against an x86 oracle.
if(VITASX2_PORTABLE_REPLAY_VALIDATION)
	add_executable(vitasx2-sotc-portable-replay
		core/PortableReplayMain.cpp
	)
	target_link_libraries(vitasx2-sotc-portable-replay
		pcsx2core
		LZMA::LZMA
	)

	vita_create_self(vitasx2-sotc-portable-replay.self vitasx2-sotc-portable-replay UNSAFE)
	vita_create_vpk(vitasx2-sotc-portable-replay.vpk ${VITASX2_TITLEID} vitasx2-sotc-portable-replay.self
		VERSION "00.01"
		NAME "VitaSX2 SOTC Replay"
	)
endif()

# --- startup/runtime validation ---------------------------------------------
add_executable(vitasx2-startup-validation
	runtime/StartupValidationMain.cpp
)
target_link_libraries(vitasx2-startup-validation
	common
)

vita_create_self(vitasx2-startup-validation.self vitasx2-startup-validation UNSAFE)
vita_create_vpk(vitasx2-startup-validation.vpk ${VITASX2_TITLEID} vitasx2-startup-validation.self
	VERSION "00.01"
	NAME "VitaSX2 Startup"
)

# --- A32 generated-code validation ------------------------------------------
add_executable(vitasx2-a32-emitter-validation
	core/A32EmitterValidationMain.cpp
)
target_link_libraries(vitasx2-a32-emitter-validation
	pcsx2core
)

vita_create_self(vitasx2-a32-emitter-validation.self vitasx2-a32-emitter-validation UNSAFE)
vita_create_vpk(vitasx2-a32-emitter-validation.vpk ${VITASX2_A32_EMITTER_VALIDATION_TITLEID} vitasx2-a32-emitter-validation.self
	VERSION "00.01"
	NAME "VitaSX2 A32 Code"
)

# --- A32 EE provider validation ---------------------------------------------
add_executable(vitasx2-a32-provider-validation
	core/A32ProviderValidationMain.cpp
)
target_link_libraries(vitasx2-a32-provider-validation
	pcsx2core
)

vita_create_self(vitasx2-a32-provider-validation.self vitasx2-a32-provider-validation UNSAFE)
vita_create_vpk(vitasx2-a32-provider-validation.vpk ${VITASX2_A32_PROVIDER_VALIDATION_TITLEID} vitasx2-a32-provider-validation.self
	VERSION "00.01"
	NAME "VitaSX2 A32 EE"
)

# --- interpreter-core memory/vtlb validation --------------------------------
add_executable(vitasx2-core-validation
	core/CoreValidationMain.cpp
)
target_link_libraries(vitasx2-core-validation
	pcsx2core
)

vita_create_self(vitasx2-core-validation.self vitasx2-core-validation UNSAFE)
vita_create_vpk(vitasx2-core-validation.vpk ${VITASX2_CORE_VALIDATION_TITLEID} vitasx2-core-validation.self
	VERSION "00.01"
	NAME "VitaSX2 Core"
)

# --- BIOS/reset validation --------------------------------------------------
add_executable(vitasx2-bios-validation
	core/BiosValidationMain.cpp
)
target_link_libraries(vitasx2-bios-validation
	pcsx2core
)

vita_create_self(vitasx2-bios-validation.self vitasx2-bios-validation UNSAFE)
vita_create_vpk(vitasx2-bios-validation.vpk ${VITASX2_BIOS_VALIDATION_TITLEID} vitasx2-bios-validation.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS"
)

# --- BIOS EE trace validation -----------------------------------------------
add_executable(vitasx2-bios-ee-trace
	core/BiosEeTraceMain.cpp
)
target_link_libraries(vitasx2-bios-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-bios-ee-trace.self vitasx2-bios-ee-trace UNSAFE)
vita_create_vpk(vitasx2-bios-ee-trace.vpk ${VITASX2_BIOS_EE_TRACE_TITLEID} vitasx2-bios-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS EE Trace"
)

# --- BIOS EE A32-provider trace validation -----------------------------------
add_executable(vitasx2-bios-ee-a32-trace
	core/BiosEeTraceMain.cpp
)
target_compile_definitions(vitasx2-bios-ee-a32-trace PRIVATE
	VITASX2_BIOS_EE_TRACE_A32=1
)
target_link_libraries(vitasx2-bios-ee-a32-trace
	pcsx2core
)

vita_create_self(vitasx2-bios-ee-a32-trace.self vitasx2-bios-ee-a32-trace UNSAFE)
vita_create_vpk(vitasx2-bios-ee-a32-trace.vpk ${VITASX2_BIOS_EE_A32_TRACE_TITLEID} vitasx2-bios-ee-a32-trace.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS EE A32 Trace"
)

# --- BIOS IOP trace validation ----------------------------------------------
add_executable(vitasx2-bios-iop-trace
	core/BiosIopTraceMain.cpp
)
target_link_libraries(vitasx2-bios-iop-trace
	pcsx2core
)

vita_create_self(vitasx2-bios-iop-trace.self vitasx2-bios-iop-trace UNSAFE)
vita_create_vpk(vitasx2-bios-iop-trace.vpk ${VITASX2_BIOS_IOP_TRACE_TITLEID} vitasx2-bios-iop-trace.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS IOP Trace"
)

# --- Homebrew ELF EE trace validation ---------------------------------------
add_executable(vitasx2-cube-ee-trace
	core/ElfEeTraceMain.cpp
)
target_compile_definitions(vitasx2-cube-ee-trace PRIVATE
	VITASX2_EE_TRACE_RECORDS=${VITASX2_CUBE_EE_TRACE_RECORDS}
	VITASX2_EE_TRACE_SKIP_RECORDS=${VITASX2_CUBE_EE_TRACE_SKIP_RECORDS}
	VITASX2_EE_TRACE_FROM_BOOT=$<BOOL:${VITASX2_CUBE_EE_TRACE_FROM_BOOT}>
)
target_link_libraries(vitasx2-cube-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-cube-ee-trace.self vitasx2-cube-ee-trace UNSAFE)
vita_create_vpk(vitasx2-cube-ee-trace.vpk ${VITASX2_CUBE_EE_TRACE_TITLEID} vitasx2-cube-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 Cube EE Trace"
)

# --- Homebrew ELF EE A32-provider trace validation ----------------------------
add_executable(vitasx2-cube-ee-a32-trace
	core/ElfEeTraceMain.cpp
)
target_compile_definitions(vitasx2-cube-ee-a32-trace PRIVATE
	VITASX2_EE_TRACE_RECORDS=${VITASX2_CUBE_EE_TRACE_RECORDS}
	VITASX2_EE_TRACE_SKIP_RECORDS=${VITASX2_CUBE_EE_TRACE_SKIP_RECORDS}
	VITASX2_EE_TRACE_FROM_BOOT=$<BOOL:${VITASX2_CUBE_EE_TRACE_FROM_BOOT}>
	VITASX2_EE_TRACE_A32=1
)
target_link_libraries(vitasx2-cube-ee-a32-trace
	pcsx2core
)

vita_create_self(vitasx2-cube-ee-a32-trace.self vitasx2-cube-ee-a32-trace UNSAFE)
vita_create_vpk(vitasx2-cube-ee-a32-trace.vpk ${VITASX2_CUBE_EE_A32_TRACE_TITLEID} vitasx2-cube-ee-a32-trace.self
	VERSION "00.01"
	NAME "VitaSX2 Cube EE A32 Trace"
)

# --- Real-game disc EE trace validation -------------------------------------
add_executable(vitasx2-simple2000-ee-trace
	core/DiscEeTraceMain.cpp
)
target_compile_definitions(vitasx2-simple2000-ee-trace PRIVATE
	VITASX2_EE_TRACE_RECORDS=${VITASX2_SIMPLE2000_EE_TRACE_RECORDS}
	VITASX2_EE_TRACE_SKIP_RECORDS=${VITASX2_SIMPLE2000_EE_TRACE_SKIP_RECORDS}
	VITASX2_EE_TRACE_FROM_BOOT=$<BOOL:${VITASX2_SIMPLE2000_EE_TRACE_FROM_BOOT}>
)
target_link_libraries(vitasx2-simple2000-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-simple2000-ee-trace.self vitasx2-simple2000-ee-trace UNSAFE)
vita_create_vpk(vitasx2-simple2000-ee-trace.vpk ${VITASX2_SIMPLE2000_EE_TRACE_TITLEID} vitasx2-simple2000-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 Simple2000 EE Trace"
)
