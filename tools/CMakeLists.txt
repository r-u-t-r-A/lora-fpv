add_executable(flrc_bench flrc_bench.c)
target_compile_options(flrc_bench PRIVATE ${OLV_WARNINGS})
target_link_libraries(flrc_bench PRIVATE olv_radio olv_common)
set_target_properties(flrc_bench PROPERTIES LINKER_LANGUAGE CXX)
