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
