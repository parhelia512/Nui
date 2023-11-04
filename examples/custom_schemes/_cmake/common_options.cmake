function(set_common_options TARGET)
    if (${MSVC})
        set(WARNINGS -Wmost)
    else()
        set(WARNINGS -Wall -Wextra -Wpedantic)
    endif()
    set(COMMON_OPTIONS ${WARNINGS} -fexceptions -pedantic)
    set(DEBUG_OPTIONS  -g ${COMMON_OPTIONS})
    set(RELEASE_OPTIONS -O3 ${COMMON_OPTIONS})
    target_compile_options(${TARGET} PUBLIC "$<$<CONFIG:DEBUG>:${DEBUG_OPTIONS}>")
    target_compile_options(${TARGET} PUBLIC "$<$<CONFIG:RELEASE>:${RELEASE_OPTIONS}>")
endfunction()