/**
 * @file gnmi.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNMI service header
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

#include <grpcpp/grpcpp.h>

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "commit.h"
#include "security/auth.h"
#include "utils/log.h"

/**
 * @brief The gNMI service: entry points for the Capabilities, Get, Set
 * and Subscribe RPCs. Should always run as a singleton.
 *
 */
class GNMIService final : public gnmi::gNMI::Service
{
  public:
    /**
     * @brief Construct the service on a sysrepo connection.
     *
     * @param[in] conn Sysrepo connection.
     * @param[in] auth Authentication/authorization data, shared with all
     * the RPC handlers.
     */
    GNMIService(sysrepo::Connection conn, std::shared_ptr<Auth> auth)
        : sr_con(conn), auth_(std::move(auth))
    {
        commit_state = std::make_shared<impl::Commit>(conn.sessionStart());
    }
    ~GNMIService() { SLOG_INFO("Quitting GNMI Server"); }

    /**
     * @brief Report the server capabilities (supported models/encodings).
     *
     * @param[in] context gRPC server context.
     * @param[in] request Incoming capability request.
     * @param[out] response Outgoing capability response.
     * @return gRPC status.
     */
    grpc::Status Capabilities(grpc::ServerContext *context, const gnmi::CapabilityRequest *request,
                              gnmi::CapabilityResponse *response);

    /**
     * @brief Retrieve a snapshot of the data tree.
     *
     * @param[in] context gRPC server context.
     * @param[in] request Incoming get request.
     * @param[out] response Outgoing get response.
     * @return gRPC status.
     */
    grpc::Status Get(grpc::ServerContext *context, const gnmi::GetRequest *request,
                     gnmi::GetResponse *response);

    /**
     * @brief Modify the data tree.
     *
     * @param[in] context gRPC server context.
     * @param[in] request Incoming set request.
     * @param[out] response Outgoing set response.
     * @return gRPC status.
     */
    grpc::Status Set(grpc::ServerContext *context, const gnmi::SetRequest *request,
                     gnmi::SetResponse *response);

    /**
     * @brief Handle a (streaming) subscription.
     *
     * @param[in] context gRPC server context.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @return gRPC status.
     */
    grpc::Status
    Subscribe(grpc::ServerContext *context,
              grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    /**
     * @brief Cancel all in-flight RPCs and reject any new RPCs (shutdown).
     *
     */
    static void TryCancelAll(void);

  private:
    sysrepo::Connection sr_con;  // sysrepo connection
    std::shared_ptr<Auth> auth_; // authentication/authorization data
    std::shared_ptr<impl::Commit> commit_state;
};
