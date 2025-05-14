cmake_minimum_required(VERSION 3.18.1)

project(sysrepo-gnxi)

## AVAILABLE OPTIONS FOR CMAKE:
##  DYNAMIC_LINK_GRPC = OFF     , link grpc and protobuf dynamically
##  CPACK_GENERATOR = [DEB,RPM] , package for deb/rpm

set(SYSREPO-GNXI_MAJOR_VERSION 0)
set(SYSREPO-GNXI_MINOR_VERSION 1)
set(SYSREPO-GNXI_MICRO_VERSION 0)

set(SYSREPO-GNXI_VERSION ${SYSREPO-GNXI_MAJOR_VERSION}.${SYSREPO-GNXI_MINOR_VERSION}.${SYSREPO-GNXI_MICRO_VERSION})

#C++11 is required by gNXI, we need c++14 for other stuff
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

# DEPENDENCIES
##############

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/CmakeModules")

find_package(PkgConfig) #official cmake module
find_package(Boost REQUIRED log system chrono) #just boost log, system and chrono libraries

pkg_check_modules(SYSREPOC REQUIRED sysrepo>=2.2.14 IMPORTED_TARGET)

pkg_check_modules(LIBYANGC REQUIRED libyang)
pkg_check_modules(SYSREPO REQUIRED sysrepo-cpp)
pkg_check_modules(LIBYANG REQUIRED libyang-cpp)

include(CodeCoverage)

# DEPENDENCIES & COMPILATION OF GNXI PROTO
##########################################

# set compiler and linker flags
set(RIGOROUS_C_FLAGS "-Wlogical-op -Wold-style-cast")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -std=c++14 -g ${RIGOROUS_C_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O2")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0")

#Use Boost with Dynamic libraries rather than static linkage
add_definitions(-D BOOST_LOG_DYN_LINK)

# SUBDIR
########

add_subdirectory(proto)

# COMPILER & LINKER
###################

# Generate a compile_commands.json with compile options
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set(GNXI_SRC src/security/authentication.cpp
             src/utils/log.cpp
             src/utils/sysrepo.cpp
             src/gnmi/gnmi.cpp
             src/gnmi/gnmi_server.cpp
             src/gnmi/capabilities.cpp
             src/gnmi/get.cpp
             src/gnmi/rpc.cpp
             src/gnmi/set.cpp
             src/gnmi/subscribe.cpp
             src/gnmi/confirm.cpp
             src/gnmi/encode/encode.cpp
             src/gnmi/encode/json_ietf.cpp
)

add_executable(gnxi_server ${GNXI_SRC} src/main.cpp)

target_compile_definitions(gnxi_server PRIVATE
    GNMI_LOGROTATE_SCRIPT="/usr/libexec/gnxi-server-logrotate"
    GNMI_LOG_DIR="/var/log/gnxi-server"
)

#Header file location required to build target
target_include_directories(gnxi_server
    PUBLIC #List of include dirs required to use target binary or library
        ${Boost_INCLUDE_DIRS}
        ${LIBYANG_INCLUDE_DIRS}
        ${SYSREPO_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}     #include "build" directory tree for "build/proto"
        ${CMAKE_CURRENT_SOURCE_DIR}/src #include "src" tree for <utils/> <security/>
)

#Directory path to look for libraries
link_directories(
    ${Boost_LIBRARY_DIRS}
    ${SYSREPO_LIBRARY_DIRS}
    ${SYSREPOC_LIBRARY_DIRS}
    ${LIBYANG_LIBRARY_DIRS}
    ${LIBYANGC_LIBRARY_DIRS}
)

# link gnxi_server executable with grpc, sysrepo libraries
target_link_libraries(gnxi_server gnmi
                      ${Boost_LIBRARIES}
                      ${SYSREPO_LIBRARIES}
                      ${SYSREPOC_LIBRARIES}
                      ${LIBYANG_LIBRARIES}
                      ${LIBYANGC_LIBRARIES}
)

# TEST
######

enable_testing()

set(GNXI_TEST_SRC tests/main.cpp
                  tests/capabilities.cpp
                  tests/get.cpp
                  tests/rpc.cpp
                  tests/set.cpp
                  tests/subscribe.cpp
              )

add_executable(gnxi_server_test ${GNXI_TEST_SRC} ${GNXI_SRC})
configure_file(scripts/gnxi-server-logrotate gnxi-server-logrotate COPYONLY)

set_target_properties(gnxi_server_test PROPERTIES COMPILE_FLAGS "-DTEST -O0 -fprofile-arcs -ftest-coverage")

get_filename_component(TESTS_SRC_DIR "${CMAKE_SOURCE_DIR}/tests" REALPATH)
get_filename_component(TESTS_WORKING_DIR "${CMAKE_BINARY_DIR}" REALPATH)
target_compile_definitions(gnxi_server_test PRIVATE
    TESTS_SRC_DIR="${TESTS_SRC_DIR}";
    TESTS_WORKING_DIR="${TESTS_WORKING_DIR}"
    GNMI_LOG_DIR="${TESTS_WORKING_DIR}/gnmi-logs"
    GNMI_LOGROTATE_SCRIPT="${TESTS_WORKING_DIR}/gnxi-server-logrotate"
)

add_test(NAME gnxi_server_test COMMAND ${CMAKE_BINARY_DIR}/gnxi_server_test)

# Add a timeout for tests, to prevent them from running forever
set_tests_properties(gnxi_server_test PROPERTIES TIMEOUT 300)

#Header file location required to build target
target_include_directories(gnxi_server_test
    PUBLIC #List of include dirs required to use target binary or library
        ${Boost_INCLUDE_DIRS}
        ${LIBYANG_INCLUDE_DIRS}
        ${SYSREPO_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}     #include "build" directory tree for "build/proto"
        ${CMAKE_CURRENT_SOURCE_DIR}/src #include "src" tree for <utils/> <security/>
)

# link gnxi_server executable with grpc, sysrepo libraries
target_link_libraries(gnxi_server_test gnmi
                      ${Boost_LIBRARIES}
                      ${SYSREPO_LIBRARIES}
                      ${SYSREPOC_LIBRARIES}
                      ${LIBYANG_LIBRARIES}
                      ${LIBYANGC_LIBRARIES}
                      ${SYSTEMD_LIBRARIES}
                      gcov
)

# Remove test files and auto-generated files from coverage report
set(GCOVR_EXCLUDES "'tests/*'" "'${CMAKE_CURRENT_BINARY_DIR}/*/*'")
setup_target_for_coverage_cobertura(gnxi_server_test_coverage ${CMAKE_CURRENT_BINARY_DIR}/gnxi_server_test coverage)
add_dependencies(gnxi_server_test_coverage gnxi_server_test)

# INSTALLATION
##############

install(TARGETS gnxi_server
        RUNTIME DESTINATION sbin
        DESTINATION ${CMAKE_INSTALL_SBINDIR}
)
