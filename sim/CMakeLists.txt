add_library(olv_sim STATIC channel.c)
target_include_directories(olv_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(olv_sim PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_sim PUBLIC olv_common)

add_executable(loopback loopback.c)
target_compile_options(loopback PRIVATE ${OLV_WARNINGS})
target_link_libraries(loopback PRIVATE olv_sim m)

# End-to-end M2 gate: needs ffmpeg with libx265 on the PATH.
find_program(FFMPEG ffmpeg)
if(FFMPEG)
    add_test(NAME m2_gate_lr520
             COMMAND loopback --mode lr520 --seconds 6 --gate --out ${CMAKE_BINARY_DIR}/m2_lr520)
    add_test(NAME m2_gate_hq2600
             COMMAND loopback --mode hq2600 --seconds 6 --gate --out ${CMAKE_BINARY_DIR}/m2_hq2600)
    set_tests_properties(m2_gate_lr520 m2_gate_hq2600 PROPERTIES LABELS sim TIMEOUT 300)
endif()
