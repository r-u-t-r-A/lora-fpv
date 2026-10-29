# Cross-compile for the Luckfox Pico (RV1103, Cortex-A7, uClibc) with the
# toolchain from the luckfox-pico SDK:
#
#   cmake -S . -B build-rv1103 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/luckfox-rv1103.cmake -DLUCKFOX_SDK=$HOME/luckfox-pico
#
# (or set LUCKFOX_SDK in the environment)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT LUCKFOX_SDK)
    set(LUCKFOX_SDK $ENV{LUCKFOX_SDK})
endif()
if(NOT LUCKFOX_SDK)
    message(FATAL_ERROR "set LUCKFOX_SDK to the luckfox-pico SDK checkout")
endif()
set(LUCKFOX_SDK ${LUCKFOX_SDK} CACHE PATH "luckfox-pico SDK" FORCE)
# CMake re-reads this file in its compiler-check projects; pass the SDK on.
set(ENV{LUCKFOX_SDK} ${LUCKFOX_SDK})
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LUCKFOX_SDK)

set(_tc ${LUCKFOX_SDK}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf)
set(_prefix ${_tc}/bin/arm-rockchip830-linux-uclibcgnueabihf-)
set(CMAKE_C_COMPILER ${_prefix}gcc)
set(CMAKE_CXX_COMPILER ${_prefix}g++)
set(CMAKE_SYSROOT ${_tc}/arm-rockchip830-linux-uclibcgnueabihf/sysroot)

set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
# The stock rootfs need not carry libstdc++: link the C++ runtime (RadioLib
# backend) statically.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
