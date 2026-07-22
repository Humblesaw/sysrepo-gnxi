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

#include <proto/gnxi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "utils/log.h"

class GNXIService final : public gnxi::gNXI::Service
{
  public:
    GNXIService(sysrepo::Connection conn) : sr_con(conn) {}
    ~GNXIService() { SLOG_INFO("Quitting GNXI Server"); }

    grpc::Status Rpc(grpc::ServerContext *context, const gnxi::RpcRequest *request,
                     gnxi::RpcResponse *response);

  private:
    sysrepo::Connection sr_con; // sysrepo connection
};
