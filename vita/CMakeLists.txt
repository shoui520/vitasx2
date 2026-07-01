# Vita-native layer: host runtime, probes, GXM backend, A32 emitters.

set(VITASX2_TITLEID "VSX200001")

# --- toolchain/runtime probe -------------------------------------------------
add_executable(vitasx2-probe
	probe/ProbeMain.cpp
)
target_link_libraries(vitasx2-probe
	pthread
	SceLibKernel_stub
	SceKernelThreadMgr_stub
	SceProcessmgr_stub
	SceIofilemgr_stub
	SceSysmem_stub
)

vita_create_self(vitasx2-probe.self vitasx2-probe UNSAFE)
vita_create_vpk(vitasx2-probe.vpk ${VITASX2_TITLEID} vitasx2-probe.self
	VERSION "00.01"
	NAME "VitaSX2 Probe"
)
