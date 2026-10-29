add_library(olv_httpd STATIC src/httpd.c src/sha1.c)
target_include_directories(olv_httpd PUBLIC src)
target_compile_options(olv_httpd PRIVATE ${OLV_WARNINGS})

add_executable(vrxd src/vrxd.c)
target_compile_options(vrxd PRIVATE ${OLV_WARNINGS})
target_link_libraries(vrxd PRIVATE olv_httpd olv_common)

add_executable(test_ws tests/test_ws.c)
target_compile_options(test_ws PRIVATE ${OLV_WARNINGS})
target_include_directories(test_ws PRIVATE ${CMAKE_SOURCE_DIR}/common/tests)
target_link_libraries(test_ws PRIVATE olv_httpd)
add_test(NAME test_ws COMMAND test_ws)
