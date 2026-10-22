/**
 * @file gnxi.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNXI service header
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

#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include <proto/yang_rpc.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "security/auth.h"

/**
 * @brief The YANG_RPC service: entry point for invoking YANG RPCs and
 * actions over gRPC.
 *
 */
class YangRpcService final : public yang_rpc::YANG_RPC::Service
{
  public:
    /**
     * @brief Construct the service on a sysrepo connection.
     *
     * @param[in] conn Sysrepo connection.
     * @param[in] auth Authentication used to derive the per-RPC NACM user,
     * shared with the gNMI service.
     */
    YangRpcService(sysrepo::Connection conn, std::shared_ptr<Auth> auth)
        : sr_con_(conn), auth_(std::move(auth))
    {
    }

    /**
     * @brief Invoke a YANG RPC or action.
     *
     * @param[in] context gRPC server context.
     * @param[in] request Incoming RPC request.
     * @param[out] response Outgoing RPC response.
     * @return gRPC status.
     */
    grpc::Status Rpc(grpc::ServerContext *context, const yang_rpc::RpcRequest *request,
        yang_rpc::RpcResponse *response);

  private:
    sysrepo::Connection sr_con_; // sysrepo connection
    std::shared_ptr<Auth> auth_; // authentication for the per-RPC NACM user
};
