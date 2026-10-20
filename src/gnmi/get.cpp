/**
 * @file get.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Get RPC implementation
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

#include <grpc/grpc.h>

#include "encode/encode.h"
#include "get.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/utils/exception.hpp>

#include <utils/log.h>
#include <utils/path.h>
#include <utils/utils.h>

namespace impl
{

grpc::Status Get::build_get_update_(google::protobuf::RepeatedPtrField<gnmi::Update> *update_list,
                                    const std::string &fullpath, gnmi::Encoding encoding,
                                    const std::string &origin)
{
    try
    {
        /* Get multiple subtree for YANG lists or one for other YANG types */
        auto sr_trees = sr_sess_.getData(fullpath);
        /* The path not (yet) existing isn't an error, so just return an empty set */
        if (!sr_trees.has_value())
        {
            return grpc::Status::OK;
        }
        for (auto n : sr_trees->findXPath(fullpath))
        {
            // root and wildcard paths may span modules of other origins
            if (!gnxi::data_node_in_origin(n, origin))
            {
                continue;
            }
            auto update = update_list->Add();
            gnxi::data_node_to_gnmi_path(n, gnxi::format_for_origin(origin),
                                         *update->mutable_path());
            update->mutable_path()->set_origin(origin);
            auto status = encodef_->encode(encoding, n, update->mutable_val());
            if (!status.ok())
            {
                update_list->Clear();
                return status;
            }
        }
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR("Failed getting items from sysrepo: ", exc.what());
        update_list->Clear();
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    catch (const std::runtime_error &exc)
    {
        SLOG_DEBUG("Path not found or not evaluable: \"", fullpath, "\": ", exc.what());
        update_list->Clear();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, exc.what());
    }

    return grpc::Status::OK;
}

/*
 * Build Get Notifications - Build a Notification message.
 * Contrary to Subscribe Notifications, A new notification message must be
 * created for every path of the GetRequest.
 * There can still be multiple paths in GetResponse if requested path
 * is a directory path.
 */
grpc::Status Get::build_get_notification_(gnmi::Notification *notification,
                                          const gnmi::Path &prefix, const gnmi::Path &path,
                                          gnmi::Encoding encoding,
                                          gnmi::GetRequest_DataType data_type)
{
    /* Data elements that have changed values */
    google::protobuf::RepeatedPtrField<gnmi::Update> *update_list = notification->mutable_update();
    auto ds = sysrepo::Datastore::Operational;

    /* Get time since epoch in milliseconds */
    notification->set_timestamp(get_time_nanosec());

    std::string origin;
    std::string fullpath;
    try
    {
        origin = gnxi::resolve_origin(prefix, path);
        fullpath = gnxi::gnmi_to_xpath(sr_sess_.getContext(), prefix, path);
    }
    catch (const grpc::Status &status)
    {
        SLOG_DEBUG("Invalid Get prefix or path: ", status.error_message());
        return status;
    }
    SLOG_DEBUG("GetRequest Path ", fullpath);

    // gNMI spec §2.2.2.1: the target set in the prefix of a request MUST be
    // reflected in the prefix of the corresponding response, and MUST NOT be
    // set when the request does not set it
    if (!prefix.target().empty())
    {
        notification->mutable_prefix()->set_target(prefix.target());
    }
    // the origin of the request prefix is echoed in the response prefix
    if (!prefix.origin().empty())
    {
        notification->mutable_prefix()->set_origin(origin);
    }

    if (data_type == gnmi::GetRequest_DataType_CONFIG)
        ds = sysrepo::Datastore::Running;

    SessionDsSwitcher ds_switch(sr_sess_, ds);

    return build_get_update_(update_list, fullpath, encoding, origin);
}

/* Verify request fields are correct */
static inline grpc::Status verify_get_request(const gnmi::GetRequest *request)
{
    auto status = gnmi_check_encoding(request->encoding());
    if (!status.ok())
        return status;

    if (!GetRequest_DataType_IsValid(request->type()))
    {
        SLOG_WARN("Invalid Data Type in Get Request ",
                  gnmi::GetRequest_DataType_Name(request->type()));
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            gnmi::GetRequest_DataType_Name(request->type()));
    }

    if (request->use_models_size() > 0)
    {
        SLOG_WARN("use_models unsupported, ALL are used");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "use_model feature unsupported");
    }

    if (request->extension_size() > 0)
    {
        SLOG_WARN("extension unsupported");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "extension feature unsupported");
    }

    return grpc::Status::OK;
}

/* Implement gNMI Get RPC */
grpc::Status Get::run(grpc::ServerContext *context, const gnmi::GetRequest *req,
                      gnmi::GetResponse *response)
{
    google::protobuf::RepeatedPtrField<gnmi::Notification> *notification_list;
    gnmi::Notification *notification;
    grpc::Status status;

    status = verify_get_request(req);
    if (!status.ok())
        return status;

    SLOG_DEBUG("GetRequest DataType ", gnmi::GetRequest::DataType_Name(req->type()),
               ", GetRequest Encoding ", gnmi::Encoding_Name(req->encoding()));

    // authorize
    std::vector<gnmi::Path> paths;
    for (const auto &p : req->path())
    {
        paths.push_back(p);
    }
    auth_.authorize(context, sr_sess_,
                    req->has_prefix() ? std::optional(req->prefix()) : std::nullopt, paths,
                    Auth::Access::READ_ONLY);

    /* Run through all paths */
    notification_list = response->mutable_notification();
    for (const auto &path : req->path())
    {
        notification = notification_list->Add();

        status = build_get_notification_(notification, req->prefix(), path, req->encoding(),
                                         req->type());
        if (!status.ok())
        {
            SLOG_WARN("Failed building get notification: ", status.error_message());
            return status;
        }
    }

    return grpc::Status::OK;
}

} // namespace impl
