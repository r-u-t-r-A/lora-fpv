# air with the LR2021/SX1280 radio (target), air_sim with the simulated ether (host).
add_library(olv_air STATIC src/vsrc.c src/cam_sensors.c)
target_include_directories(olv_air PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(olv_air PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_air PUBLIC olv_common m)

add_executable(air_sim src/air.c)
target_compile_options(air_sim PRIVATE ${OLV_WARNINGS})
target_link_libraries(air_sim PRIVATE olv_air olv_radio_sim olv_common m)
if(OLV_RADIO)
    add_executable(air src/air.c)
    target_compile_options(air PRIVATE ${OLV_WARNINGS})
    target_link_libraries(air PRIVATE olv_air olv_radio olv_common m)
    set_target_properties(air PROPERTIES LINKER_LANGUAGE CXX)
endif()

add_subdirectory(tests)

# olv-cam: camera -> H.265 on the RV1103 (Rockchip MPI + rkaiq), built only
# for the board and only when the SDK's media libraries are present (a full
# luckfox-pico checkout, or the CI's sparse one). Links the SDK's prebuilt
# librockit/librkaiq, which the stock image installs in /oem/usr/lib; their
# own dependencies (librockchip_mpp, librga) are resolved on the board.
if(LUCKFOX_SDK)
    set(_rockit ${LUCKFOX_SDK}/media/rockit/rockit)
    file(GLOB _aiq ${LUCKFOX_SDK}/media/isp/release_camera_engine_rkaiq_rv1106_*)
    if(EXISTS ${_rockit}/lib/lib32/librockit.so AND _aiq AND EXISTS ${_aiq}/lib/librkaiq.so)
        add_executable(olv-cam src/olv_cam.c)
        set(_ai ${_aiq}/include/rkaiq)
        target_include_directories(
            olv-cam SYSTEM PRIVATE ${_rockit}/mpi/sdk/include ${_ai} ${_ai}/uAPI2 ${_ai}/common
                                   ${_ai}/xcore ${_ai}/algos ${_ai}/iq_parser ${_ai}/iq_parser_v2
                                   ${_ai}/smartIr)
        target_compile_definitions(olv-cam PRIVATE RV1106_RV1103 UAPI2 RKAIQ)
        target_compile_options(olv-cam PRIVATE ${OLV_WARNINGS})
        target_link_libraries(olv-cam PRIVATE olv_air ${_rockit}/lib/lib32/librockit.so
                                              ${_aiq}/lib/librkaiq.so pthread)
        target_link_options(olv-cam PRIVATE -Wl,--allow-shlib-undefined -Wl,-rpath,/oem/usr/lib)
        set_target_properties(olv-cam PROPERTIES SKIP_BUILD_RPATH ON) # no build-host paths
        message(STATUS "olv-cam: building against ${_rockit} and ${_aiq}")
    else()
        message(STATUS "olv-cam: SDK media libraries not found, not building the camera program")
    endif()
endif()
