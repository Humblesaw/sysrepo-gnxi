/**
 * @file get.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Get RPC header
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

#include "encode/encode.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

namespace impl
{

/**
 * @brief Implementation of the gNMI Get RPC.
 *
 */
class Get
{
  public:
    explicit Get(sysrepo::Session sess) : sr_sess_(sess)
    {
        encodef_ = std::make_shared<Encode>(sr_sess_);
    }

    ~Get() {}

    /**
     * @brief Run the Get RPC.
     *
     * @param[in] context gRPC server context.
     * @param[in] req Incoming get request.
     * @param[out] response Outgoing get response.
     * @return gRPC status.
     */
    grpc::Status run(
        grpc::ServerContext *context, const gnmi::GetRequest *req, gnmi::GetResponse *response);

  private:
    /**
     * @brief Build one notification of the get response from the data of a
     * single request path.
     *
     * @param[out] notification Notification to fill.
     * @param[in] prefix Prefix of the request paths.
     * @param[in] path Request path to get.
     * @param[in] encoding Encoding requested by the client.
     * @param[in] data_type Data type requested by the client.
     * @return gRPC status.
     */
    grpc::Status build_get_notification_(gnmi::Notification *notification, const gnmi::Path &prefix,
        const gnmi::Path &path, gnmi::Encoding encoding, gnmi::GetRequest_DataType data_type);

    /**
     * @brief Build the updates of a notification from the data of one path.
     *
     * @param[out] update_list Update list to fill.
     * @param[in] fullpath XPath of the data to get.
     * @param[in] encoding Encoding requested by the client.
     * @param[in] origin Origin of the requested data.
     * @return gRPC status.
     */
    grpc::Status build_get_update_(google::protobuf::RepeatedPtrField<gnmi::Update> *update_list,
        const std::string &fullpath, gnmi::Encoding encoding, const std::string &origin);

  private:
    sysrepo::Session sr_sess_;        // sysrepo session
    std::shared_ptr<Encode> encodef_; // support for json ietf encoding
};

} // namespace impl
