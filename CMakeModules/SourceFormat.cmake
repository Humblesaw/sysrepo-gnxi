# format source files with clang-format

# check that format checking is available - always use before SOURCE_FORMAT
# the argument is the minimum supported clang-format version (major.minor.patch);
# with an older or missing formatter the format targets are silently disabled
macro(SOURCE_FORMAT_ENABLE)
    if(NOT ${ARGC} EQUAL 1)
        message(FATAL_ERROR "source_format_enable() needs the minimum clang-format version!")
    endif()

    set(SOURCE_FORMAT_ENABLED FALSE)
    find_program(CLANG_FORMAT NAMES clang-format)
    if(CLANG_FORMAT)
        execute_process(COMMAND ${CLANG_FORMAT} --version
                OUTPUT_VARIABLE CLANG_FORMAT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" CLANG_FORMAT_VERSION_NUMBER
                "${CLANG_FORMAT_VERSION}")
        if(CLANG_FORMAT_VERSION_NUMBER VERSION_GREATER_EQUAL ${ARGV0})
            set(SOURCE_FORMAT_ENABLED TRUE)
        else()
            message(STATUS "clang-format \"${CLANG_FORMAT_VERSION}\" too old (need version >= ${ARGV0}), format targets disabled.")
        endif()
    else()
        message(STATUS "clang-format not found, format targets disabled.")
    endif()
endmacro()

# files are expected to be a list and relative paths are resolved wtih respect to CMAKE_SOURCE_DIR
macro(SOURCE_FORMAT)
    if(NOT ${ARGC})
        message(FATAL_ERROR "source_format() needs a list of files to format!")
    endif()

    if(SOURCE_FORMAT_ENABLED)
        add_custom_target(format
                COMMAND ${CLANG_FORMAT} -i ${ARGN}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Formatting sources with ${CLANG_FORMAT} ...")

        add_custom_target(format-check
                COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ARGN}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Checking format of the sources with ${CLANG_FORMAT} ...")
    endif()
endmacro()
