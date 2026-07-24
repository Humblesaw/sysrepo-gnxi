cmake_minimum_required(VERSION 3.18.1)

project(sysrepo-gnxi)

set(SYSREPO-GNXI_MAJOR_VERSION 0)
set(SYSREPO-GNXI_MINOR_VERSION 1)
set(SYSREPO-GNXI_MICRO_VERSION 0)

set(SYSREPO-GNXI_VERSION ${SYSREPO-GNXI_MAJOR_VERSION}.${SYSREPO-GNXI_MINOR_VERSION}.${SYSREPO-GNXI_MICRO_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

# debug build by default
set(CMAKE_BUILD_TYPE Debug)

# custom cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/CMakeModules")

# options
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_COVERAGE "Build code coverage report from tests" OFF)

# generate a compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# DEPENDENCIES
##############

find_package(PkgConfig) # official cmake module

pkg_check_modules(SYSREPOC REQUIRED sysrepo)
pkg_check_modules(LIBYANGC REQUIRED libyang)
pkg_check_modules(SYSREPO REQUIRED sysrepo-cpp)
pkg_check_modules(LIBYANG REQUIRED libyang-cpp)

add_subdirectory(proto) # compile and link proto files

# COMPILER & LINKER
###################

# set compiler and linker flags
set(RIGOROUS_C_FLAGS "-Wlogical-op -Wold-style-cast")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -std=c++20 -g ${RIGOROUS_C_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O2")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0")

set(GNXI_SRC src/gnmi/encode/encode.cpp
             src/gnmi/encode/json_ietf.cpp 
             src/gnmi/capabilities.cpp
             src/gnmi/commit.cpp
             src/gnmi/get.cpp
             src/gnmi/gnmi_server.cpp
             src/gnmi/gnmi.cpp
             src/gnxi/gnxi.cpp
             src/gnxi/rpc.cpp
             src/gnmi/set.cpp
             src/gnmi/subscribe.cpp
             src/security/authentication.cpp
             src/utils/log.cpp
             src/utils/sysrepo.cpp
)

set(GNXI_HEADERS src/gnmi/encode/encode.h
                 src/gnmi/commit.h
                 src/gnmi/get.h
                 src/gnmi/gnmi.h
                 src/gnxi/gnxi.h
                 src/gnxi/rpc.h
                 src/gnmi/set.h
                 src/gnmi/subscribe.h
                 src/security/authentication.h
                 src/utils/log.h
                 src/utils/sysrepo.h
                 src/utils/utils.h
)

add_executable(${PROJECT_NAME} ${GNXI_SRC} src/main.cpp)

# header file location required to build target
target_include_directories(${PROJECT_NAME}
    PUBLIC # list of include dirs required to use target binary or library
        ${LIBYANG_INCLUDE_DIRS}
        ${SYSREPO_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src # include "src" tree for <utils/> <security/>
)

# directory path to look for libraries
link_directories(
    ${SYSREPO_LIBRARY_DIRS}
    ${SYSREPOC_LIBRARY_DIRS}
    ${LIBYANG_LIBRARY_DIRS}
    ${LIBYANGC_LIBRARY_DIRS}
)

# link main executable with sysrepo libraries (gRPC/protobuf come transitively
# via the gnmi library)
target_link_libraries(${PROJECT_NAME} gnmi
                      ${SYSREPO_LIBRARIES}
                      ${SYSREPOC_LIBRARIES}
                      ${LIBYANG_LIBRARIES}
                      ${LIBYANGC_LIBRARIES}
)

# TESTS
#######

set(GNXI_TESTS tests/test_capabilities.cpp
               tests/test_commit.cpp
               tests/test_get.cpp
               tests/test_rpc.cpp
               tests/test_set.cpp
               tests/test_subscribe.cpp
)

if (ENABLE_TESTS)
    enable_testing()

    get_filename_component(TESTS_SRC_DIR "${CMAKE_SOURCE_DIR}/tests" REALPATH)
    get_filename_component(TESTS_WORKING_DIR "${CMAKE_BINARY_DIR}" REALPATH)

    foreach(GNXI_TEST ${GNXI_TESTS})
        get_filename_component(GNXI_TEST_NAME "${GNXI_TEST}" NAME_WE)

        add_executable(${GNXI_TEST_NAME}
            ${GNXI_TEST}
            tests/test_main.cpp
            ${GNXI_SRC}
        )

        set_target_properties(${GNXI_TEST_NAME} PROPERTIES COMPILE_FLAGS "-DTEST -O0")

        target_compile_definitions(${GNXI_TEST_NAME} PRIVATE
            TESTS_SRC_DIR="${TESTS_SRC_DIR}";
            TESTS_WORKING_DIR="${TESTS_WORKING_DIR}"
        )

        add_test(NAME ${GNXI_TEST_NAME} COMMAND ${CMAKE_BINARY_DIR}/${GNXI_TEST_NAME})
        set_tests_properties(${GNXI_TEST_NAME} PROPERTIES TIMEOUT 300)

        # header file location required to build target
        target_include_directories(${GNXI_TEST_NAME}
            PUBLIC
                ${LIBYANG_INCLUDE_DIRS}
                ${SYSREPO_INCLUDE_DIRS}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_link_libraries(${GNXI_TEST_NAME} gnmi
            ${SYSREPO_LIBRARIES}
            ${SYSREPOC_LIBRARIES}
            ${LIBYANG_LIBRARIES}
            ${LIBYANGC_LIBRARIES}
        )
    endforeach()
endif()

# COVERAGE TESTS
################

if(ENABLE_COVERAGE)
    include(CodeCoverage)
    append_coverage_compiler_flags()

    set(GNXI_TEST_TARGETS)
    foreach(GNXI_TEST ${GNXI_TESTS})
        get_filename_component(GNXI_TEST_NAME "${GNXI_TEST}" NAME_WE)
        list(APPEND GNXI_TEST_TARGETS ${GNXI_TEST_NAME})
    endforeach()

    setup_target_for_coverage_gcovr_html(
        NAME coverage
        EXECUTABLE ctest
        DEPENDENCIES ${GNXI_TEST_TARGETS}
        EXCLUDE "tests/*" "${CMAKE_CURRENT_BINARY_DIR}/*"
    )
endif()

# INSTALLATION
##############

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION sbin
        DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

# FORMAT
########

add_custom_target(format
    COMMAND clang-format -i ${GNXI_HEADERS} ${GNXI_SRC} src/main.cpp
            ${GNXI_TESTS} tests/test_main.cpp tests/test_main.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting header and source files."
    USES_TERMINAL)
