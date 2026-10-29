add_library(olv_httpd STATIC src/httpd.c src/sha1.c)
target_include_directories(olv_httpd PUBLIC src)
target_compile_options(olv_httpd PRIVATE ${OLV_WARNINGS})

# vrxd with the LR2021 radio (target) and vrxd_sim with the simulated ether
# (host); both also replay captures.
set(VRXD_SRC src/vrxd.c src/rxlink.c)
add_executable(vrxd_sim ${VRXD_SRC})
target_compile_options(vrxd_sim PRIVATE ${OLV_WARNINGS})
target_link_libraries(vrxd_sim PRIVATE olv_httpd olv_radio_sim olv_common)
if(OLV_RADIO)
    add_executable(vrxd ${VRXD_SRC})
    target_compile_options(vrxd PRIVATE ${OLV_WARNINGS})
    target_link_libraries(vrxd PRIVATE olv_httpd olv_radio olv_common)
    set_target_properties(vrxd PROPERTIES LINKER_LANGUAGE CXX)
endif()

add_executable(test_ws tests/test_ws.c)
target_compile_options(test_ws PRIVATE ${OLV_WARNINGS})
target_include_directories(test_ws PRIVATE ${CMAKE_SOURCE_DIR}/common/tests)
target_link_libraries(test_ws PRIVATE olv_httpd)
add_test(NAME test_ws COMMAND test_ws)
