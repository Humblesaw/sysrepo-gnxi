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
#include <utils/utils.h>

namespace impl
{

grpc::Status Get::BuildGetUpdate(google::protobuf::RepeatedPtrField<gnmi::Update> *updateList,
                                 const std::string &fullpath, gnmi::Encoding encoding)
{
    try
    {
        /* Get multiple subtree for YANG lists or one for other YANG types */
        auto sr_trees = sr_sess.getData(fullpath);
        /* The path not (yet) existing isn't an error, so just return an empty set */
        if (!sr_trees.has_value())
        {
            return grpc::Status::OK;
        }
        for (auto n : sr_trees->findXPath(fullpath))
        {
            auto update = updateList->Add();
            xpath_to_gnmi(n.path(), *update->mutable_path());
            auto status = encodef->encode(encoding, n, update->mutable_val());
            if (!status.ok())
            {
                updateList->Clear();
                return status;
            }
        }
    }
    catch (std::invalid_argument &exc)
    {
        updateList->Clear();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, exc.what());
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR("Fail getting items from sysrepo: ", exc.what());
        updateList->Clear();
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
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
grpc::Status Get::BuildGetNotification(gnmi::Notification *notification, const gnmi::Path &prefix,
                                       const gnmi::Path &path, gnmi::Encoding encoding,
                                       gnmi::GetRequest_DataType dataType)
{
    /* Data elements that have changed values */
    google::protobuf::RepeatedPtrField<gnmi::Update> *updateList = notification->mutable_update();
    std::string fullpath = "";
    auto ds = sysrepo::Datastore::Operational;

    /* Get time since epoch in milliseconds */
    notification->set_timestamp(get_time_nanosec());

    if (prefix.elem_size() > 0 || !prefix.target().empty())
    {
        std::string str;
        try
        {
            str = gnmi_to_xpath(prefix);
        }
        catch (std::invalid_argument &exc)
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
        }
        SLOG_DEBUG("prefix is ", str);
        // gNMI spec §2.2.2.1:
        // When set in the prefix in a request, GetRequest, SetRequest or
        // SubscribeRequest, the field MUST be reflected in the prefix of the
        // corresponding GetResponse, SetResponse or SubscribeResponse by a
        // server.
        notification->mutable_prefix()->set_target(prefix.target());
        if (prefix.elem_size() > 0)
        {
            fullpath += str;
        }
    }

    try
    {
        gnmi_check_origin(prefix, path);
        fullpath += gnmi_to_xpath(path);
    }
    catch (std::invalid_argument &exc)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    SLOG_DEBUG("GetRequest Path ", fullpath);

    if (dataType == gnmi::GetRequest_DataType_CONFIG)
        ds = sysrepo::Datastore::Running;

    SessionDsSwitcher ds_switch(sr_sess, ds);

    return BuildGetUpdate(updateList, fullpath, encoding);
}

/* Verify request fields are correct */
static inline grpc::Status verifyGetRequest(const gnmi::GetRequest *request)
{
    switch (request->encoding())
    {
    case gnmi::JSON_IETF:
        break;

    default:
        SLOG_WARN("Unsupported Encoding ", Encoding_Name(request->encoding()));
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, Encoding_Name(request->encoding()));
    }

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
    google::protobuf::RepeatedPtrField<gnmi::Notification> *notificationList;
    gnmi::Notification *notification;
    grpc::Status status;

    status = verifyGetRequest(req);
    if (!status.ok())
        return status;

    SLOG_DEBUG("GetRequest DataType ", gnmi::GetRequest::DataType_Name(req->type()),
               ", GetRequest Encoding ", gnmi::Encoding_Name(req->encoding()));

    // authorize
    try
    {
        std::vector<gnmi::Path> paths;
        for (const auto &p : req->path())
        {
            paths.push_back(p);
        }
        auth_.authorize(context, sr_sess,
                        req->has_prefix() ? std::optional(req->prefix()) : std::nullopt, paths,
                        Auth::Access::ReadOnly);
    }
    catch (const grpc::Status &auth_status)
    {
        return auth_status;
    }

    /* Run through all paths */
    notificationList = response->mutable_notification();
    for (const auto &path : req->path())
    {
        notification = notificationList->Add();

        status =
            BuildGetNotification(notification, req->prefix(), path, req->encoding(), req->type());
        if (!status.ok())
        {
            SLOG_ERROR("Fail building get notification: ", status.error_message());
            return status;
        }
    }

    return grpc::Status::OK;
}

} // namespace impl
