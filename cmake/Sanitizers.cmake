option(SUBTITLER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(SUBTITLER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(SUBTITLER_ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(SUBTITLER_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()
