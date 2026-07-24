# DEPENDENCIES
##############

# minimum required versions
set(GRPC_MIN_VERSION 1.30.2)
set(PROTOBUF_MIN_VERSION 3.12.4)

# newer protobuf rejects the 3.12.4 request via its same-major-version policy
# which leaves protobuf targets undefined
find_package(Protobuf CONFIG QUIET)

# MODULE still enforces the minimum version even if the targets are already
# defined
find_package(Protobuf MODULE ${PROTOBUF_MIN_VERSION} REQUIRED)
find_package(gRPC CONFIG ${GRPC_MIN_VERSION} QUIET)

if(NOT TARGET gRPC::grpc++)
    # on Ubuntu 22.04, there is no cmake module for grpc
    pkg_check_modules(GRPC REQUIRED grpc++>=${GRPC_MIN_VERSION})
    add_library(gRPC::grpc++ INTERFACE IMPORTED GLOBAL)
    set_target_properties(gRPC::grpc++ PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${GRPC_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${GRPC_LIBRARIES}"
    )
endif()

# GENERATED SOURCE FILES
########################

# we just use FindGRPC.cmake to have PROTOBUF_GENERATE_GRPC_CPP function
find_package(GRPC)

get_filename_component(gnmi_proto "gnmi.proto" ABSOLUTE)
get_filename_component(gnmi_ext_proto "gnmi_ext.proto" ABSOLUTE)
get_filename_component(gnxi_proto "gnxi.proto" ABSOLUTE)

# official generator
protobuf_generate_cpp(gnmi_proto_srcs gnmi_proto_hdrs ${gnmi_proto})
protobuf_generate_cpp(gnmi_ext_proto_srcs gnmi_ext_proto_hdrs ${gnmi_ext_proto})
protobuf_generate_cpp(gnxi_proto_srcs gnxi_proto_hdrs ${gnxi_proto})

# custom generator: official `protobuf_generate_cpp` can't use grpc plugins
PROTOBUF_GENERATE_GRPC_CPP(gnmi_grpc_srcs gnmi_grpc_hdrs ${gnmi_proto})
PROTOBUF_GENERATE_GRPC_CPP(gnmi_ext_grpc_srcs gnmi_ext_grpc_hdrs ${gnmi_ext_proto})
PROTOBUF_GENERATE_GRPC_CPP(gnxi_grpc_srcs gnxi_grpc_hdrs ${gnxi_proto})

# LINK OPTIONS FOR GNMI-SERVER
##############################

# create a new library named gnmi
add_library(gnmi ${gnmi_grpc_srcs} ${gnmi_ext_grpc_srcs} ${gnxi_grpc_srcs}
                ${gnmi_proto_srcs} ${gnmi_ext_proto_srcs} ${gnxi_proto_srcs})

# link our new gnmi library with grpc and protobuf libraries, PUBLIC so the
# imported targets (and their include directories) propagate transitively to
# every executable that links gnmi
target_link_libraries(gnmi PUBLIC gRPC::grpc++ protobuf::libprotobuf)

# propagate the location of the generated proto headers to consumers, the
# generated files live in ${CMAKE_CURRENT_BINARY_DIR} (build/proto/), but
# sources include them as <proto/gnmi.pb.h>, so consumers need the top-level
# build directory on their include path
target_include_directories(gnmi PUBLIC ${CMAKE_BINARY_DIR})
