# Dependency subset for the Vita target. Everything here must build with
# arm-vita-eabi; desktop-only dependencies were removed at import time.

# fmt - formatting, used by Console/StringUtil/Error.
add_subdirectory(fmt EXCLUDE_FROM_ALL)

# fast_float - string->float, used by StringUtil.
add_subdirectory(fast_float EXCLUDE_FROM_ALL)

# simpleini - INI settings backend.
add_subdirectory(simpleini EXCLUDE_FROM_ALL)

# xxhash - header-only usage via 3rdparty/include (GS hashing).
add_library(xxhash INTERFACE)
target_include_directories(xxhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# rapidjson - header-only.
add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include")
