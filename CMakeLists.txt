# Host build: common/, air/ (software encoder path), vrx/, sim/ and unit tests.
# The Pico 2 firmware has its own project in firmware/modem (needs the Pico SDK).
cmake_minimum_required(VERSION 3.16)
project(open_lr_video C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(OLV_WARNINGS -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)

option(OLV_SANITIZE "Build with address + undefined behaviour sanitizers" OFF)
if(OLV_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

option(OLV_RADIO "Build the LR2021 backend and bench tools (needs the RadioLib submodule)" ON)
if(OLV_RADIO AND NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/RadioLib/CMakeLists.txt)
    message(WARNING "third_party/RadioLib missing (git submodule update --init); radio disabled")
    set(OLV_RADIO OFF)
endif()
if(OLV_RADIO)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_subdirectory(third_party)
add_subdirectory(common)
add_subdirectory(sim)
add_subdirectory(vrx)
add_subdirectory(air)
if(OLV_RADIO)
    add_subdirectory(radio)
    add_subdirectory(tools)
endif()

# Compile the browser's WASM glue on the host too, so warnings fail CI early.
add_library(olv_wasm_check OBJECT web/wasm/olv_wasm.c)
target_compile_options(olv_wasm_check PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_wasm_check PRIVATE olv_common)
