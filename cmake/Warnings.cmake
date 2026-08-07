# Shared warning configuration. Kept strict: the compiler is the cheapest reviewer.
function(manta_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
    endif()
endfunction()
