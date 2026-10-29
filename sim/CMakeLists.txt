add_library(olv_sim STATIC channel.c)
target_include_directories(olv_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(olv_sim PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_sim PUBLIC olv_common)

# Simulated radio (UDP multicast ether on loopback) for air_sim / vrxd_sim.
add_library(olv_radio_sim STATIC radio_sim.c)
target_compile_options(olv_radio_sim PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_radio_sim PUBLIC olv_sim olv_common)

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

# M1 gate on the simulated ether: real-time air_sim -> vrxd_sim, receivers
# join a running transmission. Uses the clip the LR M2 test writes.
find_package(Python3 COMPONENTS Interpreter)
if(FFMPEG AND Python3_FOUND)
    set_tests_properties(m2_gate_lr520 PROPERTIES FIXTURES_SETUP lr_clip)
    foreach(cfg "lr520;0.2" "hq2600;0.1")
        list(GET cfg 0 mode)
        list(GET cfg 1 loss)
        add_test(NAME m1_link_${mode}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m1_link.py
                         ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/m2_lr520/reference.ivf
                         --mode ${mode} --loss ${loss} --seconds 3)
        set_tests_properties(m1_link_${mode} PROPERTIES FIXTURES_REQUIRED lr_clip LABELS sim
                                                        TIMEOUT 60)
    endforeach()
endif()
