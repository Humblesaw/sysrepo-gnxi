/**
 * @file gnxi.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNXI service implementation
 *
 * @copyright
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

#include "yang_rpc.h"

#include "rpc.h"
#include <utils/utils.h>

grpc::Status YANG_RPCService::Rpc(grpc::ServerContext *context, const yang_rpc::RpcRequest *request,
                                  yang_rpc::RpcResponse *response)
{
    if (rpc_shutting_down.load())
    {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Server is shutting down");
    }

    // no Auth is wired into this service, read the username straight from
    // the auth context (empty when the server runs in insecure mode)
    std::string username;
    if (auto auth_ctx = context->auth_context())
    {
        const auto vals = auth_ctx->FindPropertyValues("username");
        if (!vals.empty())
        {
            username.assign(vals[0].data(), vals[0].length());
        }
    }
    slog::RequestScope req_scope("Rpc: " + rpc_user_desc(context, username));
    SLOG_INFO("Rpc RPC");

    const auto status =
        rpc_catch_exceptions("Rpc",
                             [&]
                             {
                                 impl::Rpc rpc(sr_con.sessionStart(sysrepo::Datastore::Running));
                                 return rpc.run(request, response);
                             });

    if (!status.ok())
    {
        SLOG_WARN("Rpc RPC failed: code ", static_cast<int>(status.error_code()), ": ",
                  status.error_message());
    }
    return status;
}
