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

add_subdirectory(third_party)
add_subdirectory(common)
add_subdirectory(sim)
add_subdirectory(vrx)
