# Applies PATCH in the current directory once (FetchContent runs it again on every configure).
execute_process(COMMAND git apply --reverse --check ${PATCH} RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(applied EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply ${PATCH} RESULT_VARIABLE r)
if(NOT r EQUAL 0)
    message(FATAL_ERROR "could not apply ${PATCH}")
endif()
