# Rockchip RV1103 / RV1106 (Luckfox Pico and most cameras built on these
# chips): ARMv7-A Cortex-A7 with NEON, hard float, uClibc, GCC 8.3 from the
# vendor SDK (tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf).
#
#   cmake -S . -B build-rv1106 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1106-uclibc.cmake \
#         [-DRV1106_TOOLCHAIN_DIR=<sdk>/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf]
#
# Without RV1106_TOOLCHAIN_DIR the compilers are taken from PATH.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(RV1106_TOOLCHAIN_DIR "$ENV{RV1106_TOOLCHAIN_DIR}" CACHE PATH
    "arm-rockchip830-linux-uclibcgnueabihf toolchain root (empty: use PATH)")
set(_rv1106_triple arm-rockchip830-linux-uclibcgnueabihf)
if(RV1106_TOOLCHAIN_DIR)
    set(_rv1106_prefix "${RV1106_TOOLCHAIN_DIR}/bin/${_rv1106_triple}-")
else()
    set(_rv1106_prefix "${_rv1106_triple}-")
endif()

set(CMAKE_C_COMPILER   "${_rv1106_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_rv1106_prefix}g++")
set(CMAKE_AR           "${_rv1106_prefix}ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_rv1106_prefix}ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${_rv1106_prefix}strip" CACHE FILEPATH "")

# Same CPU flags as the vendor SDK (sysdrv/Makefile.param).
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")

# Look for libraries and headers only in the toolchain's sysroot, programs
# only on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
