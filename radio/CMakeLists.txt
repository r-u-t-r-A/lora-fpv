# RadioLib (MIT), built with its own warnings off; our code keeps -Werror.
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/RadioLib ${CMAKE_BINARY_DIR}/RadioLib
                 EXCLUDE_FROM_ALL)
target_compile_options(RadioLib PRIVATE -w)

add_library(olv_radio STATIC linux_hal.cpp radio_lr2021.cpp)
target_include_directories(olv_radio PUBLIC ${CMAKE_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(olv_radio PRIVATE -Wall -Wextra -Werror)
target_link_libraries(olv_radio PUBLIC RadioLib)
