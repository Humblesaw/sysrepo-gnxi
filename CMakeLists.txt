cmake_minimum_required(VERSION 3.12)

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
# gNXI RPC service does not support per-module authorization
# keep it disabled in production builds unless explicitly enabled
option(ENABLE_YANG_RPC "Register the YANG_RPC service alongside gNMI" OFF)

# generate a compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# DEPENDENCIES
##############

find_package(PkgConfig) # official cmake module

# IMPORTED_TARGET creates PkgConfig::<PREFIX>
pkg_check_modules(SYSREPOC REQUIRED sysrepo IMPORTED_TARGET)
pkg_check_modules(LIBYANGC REQUIRED libyang IMPORTED_TARGET)
pkg_check_modules(SYSREPO REQUIRED sysrepo-cpp IMPORTED_TARGET)
pkg_check_modules(LIBYANG REQUIRED libyang-cpp IMPORTED_TARGET)

find_package(OpenSSL 3 REQUIRED) # for password hashing

find_program(SYSREPOCTL_EXECUTABLE sysrepoctl)

# install the user database YANG module into sysrepo at configure time
execute_process(
    COMMAND ${SYSREPOCTL_EXECUTABLE} -i "${CMAKE_SOURCE_DIR}/src/files/sysrepo-gnxi-users.yang"
    RESULT_VARIABLE _sysrepoctl_result
)
if(NOT _sysrepoctl_result EQUAL 0)
    message(FATAL_ERROR "sysrepoctl install failed with (rc=${_sysrepoctl_result})")
endif()

add_subdirectory(proto) # compile and link proto files

# COMPILER & LINKER
###################

# set compiler and linker flags
set(RIGOROUS_C_FLAGS "-Wlogical-op -Wold-style-cast")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -std=c++20 -g ${RIGOROUS_C_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O2")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0")

set(GNXI_SRC src/encode/encode.cpp
             src/encode/json_ietf.cpp
             src/gnmi/capabilities.cpp
             src/gnmi/commit.cpp
             src/gnmi/get.cpp
             src/gnmi/gnmi.cpp
             src/yang_rpc/yang_rpc.cpp
             src/yang_rpc/rpc.cpp
             src/gnmi/set.cpp
             src/gnmi/subscribe.cpp
             src/security/auth.cpp
             src/security/hash.cpp
             src/utils/log.cpp
             src/utils/sysrepo.cpp
)

set(GNXI_HEADERS src/encode/encode.h
                 src/gnmi/commit.h
                 src/gnmi/get.h
                 src/gnmi/gnmi.h
                 src/yang_rpc/yang_rpc.h
                 src/yang_rpc/rpc.h
                 src/gnmi/set.h
                 src/gnmi/subscribe.h
                 src/security/auth.h
                 src/security/hash.h
                 src/utils/log.h
                 src/utils/sysrepo.h
                 src/utils/utils.h
)

# server
add_executable(${PROJECT_NAME} ${GNXI_SRC} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src)
if(ENABLE_YANG_RPC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GNXI_SERVICE_ENABLED)
endif()

# header file location required to build target
target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src # include "src" tree for <utils/> <security/>
)

# link main executable with sysrepo libraries (gRPC/protobuf come transitively
# via the gnmi library)
target_link_libraries(${PROJECT_NAME} gnmi
                      PkgConfig::SYSREPO
                      PkgConfig::SYSREPOC
                      PkgConfig::LIBYANG
                      PkgConfig::LIBYANGC
                      OpenSSL::Crypto
)

# user database management utility
set(USERS_CLI sysrepo-gnxi-users)
add_executable(${USERS_CLI} src/users.cpp src/security/hash.cpp)
target_include_directories(${USERS_CLI} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src)

target_include_directories(${USERS_CLI}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${USERS_CLI}
                      PkgConfig::SYSREPO
                      PkgConfig::SYSREPOC
                      PkgConfig::LIBYANG
                      PkgConfig::LIBYANGC
                      OpenSSL::Crypto
)

# TESTS
#######

set(GNXI_TESTS tests/test_auth.cpp
               tests/test_capabilities.cpp
               tests/test_commit.cpp
               tests/test_get.cpp
               tests/test_set.cpp
               tests/test_subscribe.cpp
               tests/test_users.cpp
)

# the test_rpc suite exercises the gNXI service, build it only when enabled
if(ENABLE_YANG_RPC)
    list(APPEND GNXI_TESTS tests/test_rpc.cpp)
endif()

if (ENABLE_TESTS)
    enable_testing()

    configure_file(tests/config.h.in tests/config.h @ONLY)

    foreach(GNXI_TEST ${GNXI_TESTS})
        get_filename_component(GNXI_TEST_NAME "${GNXI_TEST}" NAME_WE)

        add_executable(${GNXI_TEST_NAME}
            ${GNXI_TEST}
            tests/test_main.cpp
        )

        set_target_properties(${GNXI_TEST_NAME} PROPERTIES
            COMPILE_FLAGS "-DTEST -O0"
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        )
        target_include_directories(${GNXI_TEST_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tests)
        if(ENABLE_YANG_RPC)
            target_compile_definitions(${GNXI_TEST_NAME} PRIVATE GNXI_SERVICE_ENABLED)
        endif()
        if("${GNXI_TEST_NAME}" STREQUAL "test_auth")
            target_compile_definitions(${GNXI_TEST_NAME} PRIVATE AUTH_MTLS_ENABLED)
        endif()

        # tests launch the server and CLI binaries as subprocesses
        add_dependencies(${GNXI_TEST_NAME} ${PROJECT_NAME} ${USERS_CLI})

        add_test(NAME ${GNXI_TEST_NAME} COMMAND ${CMAKE_BINARY_DIR}/tests/${GNXI_TEST_NAME})
        set_tests_properties(${GNXI_TEST_NAME} PROPERTIES TIMEOUT 900)

        # header file location required to build target
        target_include_directories(${GNXI_TEST_NAME}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_link_libraries(${GNXI_TEST_NAME} gnmi
            PkgConfig::SYSREPO
            PkgConfig::SYSREPOC
            PkgConfig::LIBYANG
            PkgConfig::LIBYANGC
            OpenSSL::Crypto
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

# server
install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION sbin
        DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

# user database management utility
install(TARGETS ${USERS_CLI}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# FORMAT
########

add_custom_target(format
    COMMAND clang-format -i ${GNXI_HEADERS} ${GNXI_SRC} src/main.cpp src/users.cpp
            ${GNXI_TESTS} tests/test_main.cpp tests/test_main.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting header and source files."
    USES_TERMINAL)
