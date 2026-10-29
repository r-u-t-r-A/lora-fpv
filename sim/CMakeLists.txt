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
    # 2.4 GHz-only links. E28 (SX1280): 127-byte packets, VRX on mode=auto,
    # which on that chip can only be hq1300.
    add_test(NAME m1_link_e28
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m1_link.py
                     ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/m2_lr520/reference.ivf
                     --mode hq1300 --loss 0.1 --seconds 3 --radio module=e28-2g4m27s
                     --vrx-mode auto)
    # LR2021 VRX told band=2400, mode=auto: scans hq2600 and hq1300. Worst
    # case one wasted hq2600 park (41 hops) plus acquisition (79 hops).
    add_test(NAME m1_scan_2g4
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m1_link.py
                     ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/m2_lr520/reference.ivf
                     --mode hq1300 --loss 0.1 --seconds 4 --vrx-mode auto --vrx-band 2400
                     --lock-limit-ms 2000)
    set_tests_properties(m1_link_e28 m1_scan_2g4 PROPERTIES FIXTURES_REQUIRED lr_clip LABELS sim
                                                            TIMEOUT 60)
    # Camera source: sim/fake_cam.py stands in for olv-cam, dies, the fallback
    # takes over, the camera comes back.
    add_test(NAME cam_source
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cam_source.py
                     ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/m2_lr520/reference.ivf)
    set_tests_properties(cam_source PROPERTIES FIXTURES_REQUIRED lr_clip LABELS sim TIMEOUT 60)
endif()
