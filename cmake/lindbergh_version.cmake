# Writes OUT with LINDBERGH_RUNTIME_REV: the runtime's git commit (short),
# "-dirty" when its tracked files have changes. Rewritten only when it changes,
# so a build that changes nothing recompiles nothing.
execute_process(COMMAND git -C "${SRC}" rev-parse --short=10 HEAD
                OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR rev STREQUAL "")
    set(rev "unknown")
else()
    execute_process(COMMAND git -C "${SRC}" diff --quiet HEAD -- src third_party
                    RESULT_VARIABLE dirty ERROR_QUIET)
    if(NOT dirty EQUAL 0)
        set(rev "${rev}-dirty")
    endif()
endif()
set(text "#define LINDBERGH_RUNTIME_REV \"${rev}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
else()
    set(old "")
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
