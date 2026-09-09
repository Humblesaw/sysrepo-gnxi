cmake_minimum_required(VERSION 3.12)

set(SYSREPO-GNXI_MAJOR_VERSION 0)
set(SYSREPO-GNXI_MINOR_VERSION 1)
set(SYSREPO-GNXI_MICRO_VERSION 0)

project(sysrepo-gnxi
        VERSION ${SYSREPO-GNXI_MAJOR_VERSION}.${SYSREPO-GNXI_MINOR_VERSION}.${SYSREPO-GNXI_MICRO_VERSION}
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

# debug build by default, override with -DCMAKE_BUILD_TYPE=<type>
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

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
pkg_check_modules(SYSREPOC REQUIRED sysrepo>=5.2.17 IMPORTED_TARGET)
pkg_check_modules(LIBYANGC REQUIRED libyang>=6.2.8 IMPORTED_TARGET)
pkg_check_modules(SYSREPO REQUIRED sysrepo-cpp>=10 IMPORTED_TARGET)
pkg_check_modules(LIBYANG REQUIRED libyang-cpp>=11 IMPORTED_TARGET)

# for password hashing
find_package(OpenSSL 3 REQUIRED)
pkg_check_modules(LIBCRYPT REQUIRED libcrypt IMPORTED_TARGET)

add_subdirectory(proto) # compile and link proto files

# dependencies shared by all sysrepo-gnxi targets
# (gRPC/protobuf come transitively via the gnmi library)
add_library(gnxi_deps INTERFACE)
target_link_libraries(gnxi_deps INTERFACE
    gnmi
    PkgConfig::SYSREPO
    PkgConfig::SYSREPOC
    PkgConfig::LIBYANG
    PkgConfig::LIBYANGC
    PkgConfig::LIBCRYPT
    OpenSSL::Crypto
)

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
             src/security/crypto.cpp
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
                 src/security/crypto.h
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

target_link_libraries(${PROJECT_NAME} gnxi_deps)

# user database management utility
set(USERS_CLI sysrepo-gnxi-users)
add_executable(${USERS_CLI} src/users.cpp src/security/hash.cpp src/security/crypto.cpp)
target_include_directories(${USERS_CLI} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src ${CMAKE_BINARY_DIR})

target_include_directories(${USERS_CLI}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${USERS_CLI} gnxi_deps)

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

        target_link_libraries(${GNXI_TEST_NAME} gnxi_deps)
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

# install the YANG modules into sysrepo using a script
option(SYSREPO_SETUP "Install required YANG modules into sysrepo using a script" ON)
set(GNXI_YANG_MODULE_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/sysrepo-gnxi/modules"
    CACHE STRING "Directory where to copy the YANG modules to")
set(GNXI_SCRIPTS_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/sysrepo-gnxi"
    CACHE STRING "Directory where to copy the helper scripts to")

# server
install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION sbin
        DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

# user database management utility
install(TARGETS ${USERS_CLI}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# YANG modules
install(DIRECTORY "${CMAKE_SOURCE_DIR}/modules/"
        DESTINATION ${GNXI_YANG_MODULE_DIR}
)

# helper scripts and files
install(PROGRAMS scripts/setup.sh
        DESTINATION ${GNXI_SCRIPTS_DIR}
)
install(FILES example_config/minimal-configuration.json
        DESTINATION ${GNXI_SCRIPTS_DIR}
)

if(SYSREPO_SETUP)
    find_program(SYSREPOCTL_EXECUTABLE sysrepoctl)
    if(NOT SYSREPOCTL_EXECUTABLE)
        message(FATAL_ERROR "Unable to find sysrepoctl, set SYSREPOCTL_EXECUTABLE manually.")
    endif()
    find_program(SYSREPOCFG_EXECUTABLE sysrepocfg)
    if(NOT SYSREPOCFG_EXECUTABLE)
        message(FATAL_ERROR "Unable to find sysrepocfg, set SYSREPOCFG_EXECUTABLE manually.")
    endif()

    # install the modules into sysrepo using the installed setup.sh,
    # the module directory is passed via environment variables (like netopeer2)
    install(CODE "
        message(STATUS \"Installing YANG modules into sysrepo (setup.sh)...\")
        set(ENV{GNXI_MODULE_DIR} \"${GNXI_YANG_MODULE_DIR}\")
        set(ENV{SYSREPOCTL_EXECUTABLE} \"${SYSREPOCTL_EXECUTABLE}\")
        set(ENV{SYSREPOCFG_EXECUTABLE} \"${SYSREPOCFG_EXECUTABLE}\")
        set(ENV{GNXI_CONFIG_FILE} \"${GNXI_SCRIPTS_DIR}/minimal-configuration.json\")
        execute_process(COMMAND \"\$ENV{DESTDIR}${GNXI_SCRIPTS_DIR}/setup.sh\"
            RESULT_VARIABLE CMD_RES
            OUTPUT_VARIABLE CMD_OUT
            ERROR_VARIABLE CMD_ERR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT CMD_RES EQUAL 0)
            string(REPLACE \"\\n\" \"\\n \" CMD_OUT_F \"\${CMD_OUT}\")
            string(REPLACE \"\\n\" \"\\n \" CMD_ERR_F \"\${CMD_ERR}\")
            message(FATAL_ERROR \" OUTPUT:\\n \${CMD_OUT_F}\\n ERROR:\\n \${CMD_ERR_F}\")
        endif()
    ")
else()
    message(WARNING "Server will refuse to start if the YANG modules are not installed!")
endif()

# FORMAT
########

add_custom_target(format
    COMMAND clang-format -i ${GNXI_HEADERS} ${GNXI_SRC} src/main.cpp src/users.cpp
            ${GNXI_TESTS} tests/test_main.cpp tests/test_main.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting header and source files."
    USES_TERMINAL)
