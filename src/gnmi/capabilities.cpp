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

grpc::Status GNMIService::Capabilities(grpc::ServerContext *context,
                                       const gnmi::CapabilityRequest *request,
                                       gnmi::CapabilityResponse *response)
{
    (void)context;
    std::string gnmi_version;

    if (request->extension_size() > 0)
    {
        SLOG_ERROR("Extensions not implemented");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Extensions not implemented");
    }

    try
    {
        auto sess = sr_con.sessionStart();

        for (auto module : sess.getContext().modules())
        {
            if (module.implemented())
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
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR(exc.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Fail getting schemas");
    }

    return grpc::Status::OK;
}
