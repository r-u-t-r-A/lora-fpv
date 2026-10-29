# zfex: vendored, GPL-2.0-or-later (see zfex/copyright). Built with its own,
# relaxed warning set; our code is held to -Werror, upstream code is not.
add_library(zfex STATIC zfex/zfex.c)
target_include_directories(zfex PUBLIC zfex)
target_compile_options(zfex PRIVATE -w)
