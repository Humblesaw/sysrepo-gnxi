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

class Subscribe
{
  public:
    Subscribe(sysrepo::Session sess, const Auth &auth) : sr_sess(sess), auth_(auth)
    {
        encodef = std::make_shared<Encode>(sr_sess);
    }
    ~Subscribe() {}

    grpc::Status
    run(grpc::ServerContext *context,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    void
    streamWorker(grpc::ServerContext *context, gnmi::SubscribeRequest request,
                 grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
                 Scheduler &scheduler);
    void triggerSampleUpdate(
        grpc::ServerContext *context, std::shared_ptr<gnmi::Subscription> &sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        gnmi::Encoding encoding);
    grpc::Status BuildSubscribeNotification(gnmi::Notification *notification,
                                            const gnmi::SubscriptionList &request,
                                            bool *sample = nullptr);
    grpc::Status BuildSubscribeNotificationForChanges(gnmi::Notification *notification,
                                                      const gnmi::SubscriptionList &request,
                                                      std::string &xpath, sysrepo::Session session);
    // To synchronize write access to the stream
    void Write(grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
               gnmi::SubscribeResponse response);

  private:
    grpc::Status BuildSubsUpdate(google::protobuf::RepeatedPtrField<gnmi::Update> *updateList,
                                 const gnmi::Path &prefix, std::string fullpath,
                                 gnmi::Encoding encoding);
    grpc::Status registerStreamOnChange(
        gnmi::SubscribeRequest &request, gnmi::Subscription sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Scheduler &scheduler, std::shared_ptr<DataSubscribe> sr_sub,
        std::vector<SrModuleOnChangeParams> &params_vec);
    grpc::Status
    handleStream(grpc::ServerContext *context, gnmi::SubscribeRequest request,
                 grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);
    grpc::Status
    handleOnce(gnmi::SubscribeRequest request,
               grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);
    grpc::Status
    handlePoll(gnmi::SubscribeRequest request,
               grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

  private:
    sysrepo::Session sr_sess;        // sysrepo session
    std::shared_ptr<Encode> encodef; // support for json ietf encoding
    const Auth &auth_;               // authentication/authorization data
    std::recursive_mutex stream_mutex;
};

} // namespace impl
