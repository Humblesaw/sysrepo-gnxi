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

#include <grpcpp/grpcpp.h>

#include <proto/yang_rpc.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "utils/log.h"

class YANG_RPCService final : public yang_rpc::YANG_RPC::Service
{
  public:
    YANG_RPCService(sysrepo::Connection conn) : sr_con(conn) {}
    ~YANG_RPCService() { SLOG_INFO("Quitting GNXI Server"); }

    grpc::Status Rpc(grpc::ServerContext *context, const yang_rpc::RpcRequest *request,
                     yang_rpc::RpcResponse *response);

  private:
    sysrepo::Connection sr_con; // sysrepo connection
};
