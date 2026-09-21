/**
 * @file capabilities.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Capabilities RPC implementation
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

#include <proto/gnmi.grpc.pb.h>

#include "gnmi.h"
#include "utils/log.h"
#include "utils/utils.h"

grpc::Status GNMIService::Capabilities(grpc::ServerContext *context,
                                       const gnmi::CapabilityRequest *request,
                                       gnmi::CapabilityResponse *response)
{
    std::string gnmi_version;

    if (rpc_shutting_down.load())
    {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Server is shutting down");
    }

    slog::RequestScope req_scope("Capabilities: " +
                                 rpc_user_desc(context, auth_->username(context)));
    SLOG_INFO("Capabilities RPC");

    if (request->extension_size() > 0)
    {
        SLOG_WARN("Capabilities RPC failed: extensions not implemented");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Extensions not implemented");
    }

    const auto status = rpc_catch_exceptions(
        "Capabilities",
        [&]() -> grpc::Status
        {
            auto sess = sr_con.sessionStart();

            for (auto module : sess.getContext().modules())
            {
                if (module.implemented() && !isPrivateModule(module.name()))
                {
                    auto model = response->add_supported_models();
                    model->set_name(module.name());
                    model->set_organization(module.org().value_or(""));
                    model->set_version(module.revision().value_or(""));
                }
            }

            gnmi_version =
                response->GetDescriptor()->file()->options().GetExtension(gnmi::gnmi_service);
            response->set_gnmi_version(gnmi_version);

            // Encoding used in TypedValue for responses
            // response->add_supported_encodings(gnmi::Encoding::JSON);
            // response->add_supported_encodings(gnmi::Encoding::BYTES);
            // response->add_supported_encodings(gnmi::Encoding::PROTO);
            // response->add_supported_encodings(gnmi::Encoding::ASCII);
            response->add_supported_encodings(gnmi::Encoding::JSON_IETF);

            return grpc::Status::OK;
        });

    if (!status.ok())
    {
        SLOG_WARN("Capabilities RPC failed: code ", static_cast<int>(status.error_code()), ": ",
                  status.error_message());
    }
    else
    {
        SLOG_INFO("Capabilities RPC succeeded, ", response->supported_models_size(),
                  " supported models");
    }
    return status;
}
