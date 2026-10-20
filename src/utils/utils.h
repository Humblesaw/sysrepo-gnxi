/**
 * @file utils.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Utilities implementation
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
 * Copyright 2025 Graphiant Inc.
 * Copyright (c) 2026 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <grpc/status.h>
#include <grpcpp/server_context.h>
#include <proto/gnmi.grpc.pb.h>
#include <string>
#include <unordered_set>

#include "utils/log.h"

/* Get current time since epoch in nanosec */
inline uint64_t get_time_nanosec()
{
    std::chrono::nanoseconds ts;
    ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    return ts.count();
}

/**
 * @brief Check whether a YANG module name belongs to a private module
 * that must never be accessible via gNMI RPCs.
 *
 * The set of private modules is hardcoded. It includes all modules that
 * store server configuration, TLS credentials or user database data,
 * and the internal "sysrepo" module (datastore internals).
 *
 * @param[in] name Module name to check.
 * @return True if the module is private, false otherwise.
 */
inline bool is_private_module(const std::string &name)
{
    static const std::unordered_set<std::string> PRIVATE_MODULES = {
        "sysrepo",
        "sysrepo-gnxi-server",
        "sysrepo-gnxi-users",
        "ietf-keystore",
        "ietf-truststore",
        "ietf-crypto-types",
        "ietf-tls-server",
        "ietf-tls-common",
        "iana-tls-cipher-suite-algs",
    };
    return PRIVATE_MODULES.contains(name);
}

/**
 * @brief One-way server shutdown flag shared by all gRPC services.
 *
 * Set when the server starts shutting down and never cleared again -
 * restarting the services within the same process is not supported. New
 * RPCs are rejected with UNAVAILABLE once it is set.
 */
inline std::atomic<bool> rpc_shutting_down{false};

/**
 * @brief Describe the calling user and peer of an RPC for audit logging.
 *
 * @param[in] context Server context of the RPC.
 * @param[in] username Authenticated username (empty when unavailable/insecure).
 * @return "user '<name>' from <peer>", with '<insecure>' when there is no user.
 */
inline std::string rpc_user_desc(grpc::ServerContext *context, const std::string &username)
{
    std::string desc = "user '";
    desc += username.empty() ? "<insecure>" : username;
    desc += "' from ";
    desc += context->peer();
    return desc;
}

/**
 * @brief Central exception handler for gRPC service methods.
 *
 * Wraps an RPC handler invocation so that no exception can escape into
 * the gRPC runtime: grpc::Status exceptions are returned as-is,
 * std::exception becomes INTERNAL. Local try-catch blocks are then only
 * needed where exceptions are mapped to more specific status codes or
 * where cleanup is required.
 *
 * @param[in] rpc_name Name of the RPC being handled (used in log messages).
 * @param[in] handler Callable performing the actual RPC handling.
 * @return The status returned by the handler or its exception's status.
 */
template <typename Handler>
grpc::Status rpc_catch_exceptions(const char *rpc_name, Handler &&handler)
{
    try
    {
        return handler();
    }
    catch (const grpc::Status &status)
    {
        return status;
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR("Unexpected error in ", rpc_name, " RPC handling: ", exc.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
    }
    catch (...)
    {
        SLOG_ERROR("Unexpected unknown error in ", rpc_name, " RPC handling");
        return grpc::Status(grpc::StatusCode::INTERNAL, "unknown error");
    }
}

/**
 * @brief Check that the gNMI encoding of a request is supported.
 *
 * @param[in] encoding Encoding requested by the client.
 * @return grpc::Status::OK when supported, UNIMPLEMENTED naming the
 * encoding otherwise.
 */
inline grpc::Status gnmi_check_encoding(gnmi::Encoding encoding)
{
    if (encoding != gnmi::JSON_IETF)
    {
        auto name = gnmi::Encoding_Name(encoding);
        SLOG_WARN("Unsupported Encoding ", name);
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, name);
    }
    return grpc::Status::OK;
}
