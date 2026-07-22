/*
 * Copyright 2020 Yohan Pipereau
 * Copyright 2025 Graphiant Inc.
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

#include <future>

#include <grpcpp/grpcpp.h>

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "commit.h"
#include "utils/log.h"

class GNMIService final : public gnmi::gNMI::Service
{
  public:
    GNMIService(sysrepo::Connection conn) : sr_con(conn)
    {
        commit_state = std::make_shared<impl::Commit>(conn.sessionStart());
    }
    ~GNMIService() { SLOG_INFO("Quitting GNMI Server"); }

    grpc::Status Capabilities(grpc::ServerContext *context, const gnmi::CapabilityRequest *request,
                              gnmi::CapabilityResponse *response);

    grpc::Status Get(grpc::ServerContext *context, const gnmi::GetRequest *request,
                     gnmi::GetResponse *response);

    grpc::Status Set(grpc::ServerContext *context, const gnmi::SetRequest *request,
                     gnmi::SetResponse *response);

    grpc::Status
    Subscribe(grpc::ServerContext *context,
              grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    static void TryCancelAll(void);

  private:
    // void ServerContextUpdate(grpc::ServerContext *ctx, bool add); UNUSED
    sysrepo::Connection sr_con; // sysrepo connection
    std::shared_ptr<impl::Commit> commit_state;
};

void RunServer(std::string bind_addr, std::shared_ptr<grpc::ServerCredentials> cred,
               sysrepo::Connection sr_conn, std::promise<void> ready = std::promise<void>());

void SetupSignalHandler(bool daemon = true);
