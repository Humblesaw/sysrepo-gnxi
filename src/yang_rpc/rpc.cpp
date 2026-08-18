/**
 * @file rpc.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Rpc RPC implementation
 *
 * @copyright
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

#include <grpc/grpc.h>
#include <proto/yang_rpc.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "rpc.h"
#include <sysrepo-cpp/utils/exception.hpp>
#include <utils/log.h>
#include <utils/utils.h>

namespace impl
{

grpc::Status Rpc::run(const yang_rpc::RpcRequest *request, yang_rpc::RpcResponse *response)
{
    try
    {
        auto xpath = gnmi_to_xpath(request->path());
        auto [status, input_node] = encodef->decode(xpath, request->input(), EncodePurpose::Rpc);
        if (!status.ok())
        {
            SLOG_WARN("Rpc input value error: ", status.error_message());
            return status;
        }

        auto output_node = sr_sess.sendRPC(input_node.value());

        response->set_timestamp(get_time_nanosec());

        if (output_node.has_value())
        {
            status = encodef->encode(request->encoding(), output_node.value(),
                                     response->mutable_output());
            if (!status.ok())
            {
                SLOG_WARN("Rpc output value error: ", status.error_message());
                return status;
            }
        }

        return grpc::Status::OK;
    }
    catch (libyang::ErrorWithCode &e)
    {
        SLOG_WARN("RPC error: ", e.what());
        return grpc::Status(grpc::StatusCode::ABORTED, e.what());
    }
    catch (sysrepo::ErrorWithCode &e)
    {
        SLOG_WARN("RPC error: ", e.what());
        switch (e.code())
        {
        // TODO - unautorized is never executed -> server does not set file permissions for now!
        case sysrepo::ErrorCode::Unauthorized:
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, e.what());
        case sysrepo::ErrorCode::InvalidArgument:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
        case sysrepo::ErrorCode::NotFound:
            return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
        case sysrepo::ErrorCode::Timeout:
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, e.what());
        default:
            return grpc::Status(grpc::StatusCode::ABORTED, e.what());
        }
    }
    catch (grpc::Status &e)
    {
        SLOG_WARN("RPC error: ", e.error_message());
        return e;
    }
    catch (std::exception &e)
    {
        SLOG_WARN("RPC error: ", e.what());
        return grpc::Status(grpc::StatusCode::ABORTED, e.what());
    }
}

} // namespace impl
