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
option(DYNAMIC_LINK_GRPC "Link GRPC and Protobuf dynamically" ON)
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_COVERAGE "Build code coverage report from tests" OFF)

# generate a compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# DEPENDENCIES
##############

# minimum required versions
set(GRPC_MIN_VERSION 1.30.2)
set(PROTOBUF_MIN_VERSION 3.12.4)

find_package(PkgConfig) # official cmake module

# newer protobuf rejects the 3.12.4 request via its same-major-version policy
# which leaves protobuf targets undefined
find_package(Protobuf CONFIG QUIET)

# MODULE still enforces the minimum version even if the targets are already
# defined
find_package(Protobuf MODULE ${PROTOBUF_MIN_VERSION} REQUIRED)
find_package(gRPC CONFIG ${GRPC_MIN_VERSION} QUIET)

# on Ubuntu 22.04, there is no cmake module for grpc 
if(NOT gRPC_FOUND)
    pkg_check_modules(GRPC REQUIRED grpc++>=${GRPC_MIN_VERSION})
    add_library(gRPC::grpc++ INTERFACE IMPORTED)
    set_target_properties(gRPC::grpc++ PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${GRPC_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${GRPC_LIBRARIES}"
    )
endif()

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

add_executable(gnxi_server ${GNXI_SRC} src/main.cpp)

# header file location required to build target
target_include_directories(gnxi_server
    PUBLIC # list of include dirs required to use target binary or library
        ${LIBYANG_INCLUDE_DIRS}
        ${SYSREPO_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}     # include "build" directory tree for "build/proto"
        ${CMAKE_CURRENT_SOURCE_DIR}/src # include "src" tree for <utils/> <security/>
)

# directory path to look for libraries
link_directories(
    ${SYSREPO_LIBRARY_DIRS}
    ${SYSREPOC_LIBRARY_DIRS}
    ${LIBYANG_LIBRARY_DIRS}
    ${LIBYANGC_LIBRARY_DIRS}
)

# link gnxi_server executable with grpc, sysrepo libraries
target_link_libraries(gnxi_server gnmi
                      ${SYSREPO_LIBRARIES}
                      ${SYSREPOC_LIBRARIES}
                      ${LIBYANG_LIBRARIES}
                      ${LIBYANGC_LIBRARIES}
                      gRPC::grpc++
                      ${Protobuf_LIBRARIES}
)

# TESTS
#######

if (ENABLE_TESTS)
    enable_testing()
    set(GNXI_TEST_SRC tests/capabilities.cpp
                    tests/commit.cpp
                    tests/get.cpp
                    tests/main.cpp
                    tests/rpc.cpp
                    tests/set.cpp
                    tests/subscribe.cpp
    )

    add_executable(gnxi_server_test ${GNXI_TEST_SRC} ${GNXI_SRC})

    set_target_properties(gnxi_server_test PROPERTIES COMPILE_FLAGS "-DTEST -O0")

    get_filename_component(TESTS_SRC_DIR "${CMAKE_SOURCE_DIR}/tests" REALPATH)
    get_filename_component(TESTS_WORKING_DIR "${CMAKE_BINARY_DIR}" REALPATH)
    target_compile_definitions(gnxi_server_test PRIVATE
        TESTS_SRC_DIR="${TESTS_SRC_DIR}";
        TESTS_WORKING_DIR="${TESTS_WORKING_DIR}"
    )

    add_test(NAME gnxi_server_test COMMAND ${CMAKE_BINARY_DIR}/gnxi_server_test)

    # add a timeout for tests, to prevent them from running forever
    set_tests_properties(gnxi_server_test PROPERTIES TIMEOUT 300)

    # header file location required to build target
    target_include_directories(gnxi_server_test
        PUBLIC # list of include dirs required to use target binary or library
            ${LIBYANG_INCLUDE_DIRS}
            ${SYSREPO_INCLUDE_DIRS}
            ${PROTOBUF_INCLUDE_DIR}
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}     # include "build" directory tree for "build/proto"
            ${CMAKE_CURRENT_SOURCE_DIR}/src # include "src" tree for <utils/> <security/>
    )

    # link gnxi_server executable with grpc, sysrepo libraries
    target_link_libraries(gnxi_server_test gnmi
                        ${SYSREPO_LIBRARIES}
                        ${SYSREPOC_LIBRARIES}
                        ${LIBYANG_LIBRARIES}
                        ${LIBYANGC_LIBRARIES}
                        gRPC::grpc++
                        ${Protobuf_LIBRARIES}
    )
endif()

# COVERAGE TESTS
################

if(ENABLE_COVERAGE)
    include(CodeCoverage)
    append_coverage_compiler_flags()

    setup_target_for_coverage_gcovr_html(
        NAME coverage
        EXECUTABLE ${CMAKE_BINARY_DIR}/gnxi_server_test
        DEPENDENCIES gnxi_server_test
        EXCLUDE "tests/*" "${CMAKE_CURRENT_BINARY_DIR}/*"
    )
endif()

# INSTALLATION
##############

install(TARGETS gnxi_server
        RUNTIME DESTINATION sbin
        DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

# FORMAT
########

add_custom_target(format
    COMMAND clang-format -i ${GNXI_HEADERS} ${GNXI_SRC} src/main.cpp ${GNXI_TEST_SRC}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting header and source files."
    USES_TERMINAL)
