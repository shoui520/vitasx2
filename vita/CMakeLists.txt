# VitaSX2 production application.

set(VITASX2_TITLEID "VSX200001")

add_executable(vitasx2
	product/VitaSx2Main.cpp
)
target_link_libraries(vitasx2
	pcsx2core
	LZMA::LZMA
)

# VitaSX2's persistent emulator/JIT/GXM owners require the supported extended
# application-memory budget. This emits PARAM.SFO ATTRIBUTE2=12 (365 MiB).
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_self(vitasx2.self vitasx2 UNSAFE)
vita_create_vpk(vitasx2.vpk ${VITASX2_TITLEID} vitasx2.self
	VERSION "00.01"
	NAME "VitaSX2"
)
