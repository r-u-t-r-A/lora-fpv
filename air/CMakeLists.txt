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

