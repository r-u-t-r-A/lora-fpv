add_library(olv_common STATIC ota.c vrx_proto.c fhss.c fec.c h265.c vpkt.c airsched.c)
target_include_directories(olv_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(olv_common PRIVATE ${OLV_WARNINGS})
target_link_libraries(olv_common PUBLIC zfex)

add_subdirectory(tests)
