/**
 * @file subscribe.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Subscribe RPC header
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
#include "security/auth.h"
#include "utils/sysrepo.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

namespace impl
{

class SrModuleOnChangeParams;
class Scheduler;

/**
 * @brief Implementation of the gNMI Subscribe RPC.
 *
 */
class Subscribe
{
  public:
    Subscribe(sysrepo::Session sess, const Auth &auth) : sr_sess_(sess), auth_(auth)
    {
        encodef_ = std::make_shared<Encode>(sr_sess_);
    }

    ~Subscribe() {}

    grpc::Status run(grpc::ServerContext *context,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    void stream_worker(grpc::ServerContext *context, gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Scheduler &scheduler);
    void trigger_sample_update(grpc::ServerContext *context,
        std::shared_ptr<gnmi::Subscription> &sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        gnmi::Encoding encoding, const gnmi::Path &prefix);
    grpc::Status build_subscribe_notification(gnmi::Notification *notification,
        const gnmi::SubscriptionList &request, bool *sample = nullptr);
    grpc::Status build_subscribe_notification_for_changes(gnmi::Notification *notification,
        const gnmi::SubscriptionList &request, std::string &xpath, std::string origin,
        sysrepo::Session session);
    // To synchronize write access to the stream
    void write(grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        gnmi::SubscribeResponse response);

  private:
    grpc::Status build_subs_update_(google::protobuf::RepeatedPtrField<gnmi::Update> *update_list,
        std::string fullpath, gnmi::Encoding encoding, std::string origin);
    grpc::Status register_stream_on_change_(gnmi::SubscribeRequest &request, gnmi::Subscription sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Scheduler &scheduler, std::shared_ptr<DataSubscribe> sr_sub,
        std::vector<SrModuleOnChangeParams> &params_vec);
    grpc::Status handle_stream_(grpc::ServerContext *context, gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);
    grpc::Status handle_once_(gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);
    grpc::Status handle_poll_(gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

  private:
    sysrepo::Session sr_sess_;        // sysrepo session
    std::shared_ptr<Encode> encodef_; // support for json ietf encoding
    const Auth &auth_;                // authentication/authorization data
    std::recursive_mutex stream_mutex_;
};

} // namespace impl
