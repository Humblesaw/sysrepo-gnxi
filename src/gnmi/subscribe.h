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

    /**
     * @brief Run the Subscribe RPC.
     *
     * @param[in] context gRPC server context.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @return gRPC status.
     */
    grpc::Status run(grpc::ServerContext *context,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    /**
     * @brief Worker loop scheduling the subscription events of a stream subscription.
     *
     * @param[in] context gRPC server context.
     * @param[in] request Subscription request this worker serves.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @param[in,out] scheduler Scheduler multiplexing the subscription events.
     */
    void stream_worker(grpc::ServerContext *context, gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Scheduler &scheduler);

    /**
     * @brief Fetch the current data of a sample subscription and write them to the stream.
     *
     * @param[in] context gRPC server context.
     * @param[in] sub Sample subscription to update.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @param[in] encoding Encoding of the original subscription list.
     * @param[in] prefix Prefix of the original subscription list.
     */
    void trigger_sample_update(grpc::ServerContext *context,
        std::shared_ptr<gnmi::Subscription> &sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        gnmi::Encoding encoding, const gnmi::Path &prefix);

    /**
     * @brief Build a notification with the current data of all subscriptions
     * of a subscription list (initial update).
     *
     * @param[out] notification Notification to fill.
     * @param[in] request Subscription list to answer to.
     * @param[out] sample Set to true when the list holds at least one sample
     * subscription to serve, may be nullptr.
     * @return gRPC status.
     */
    grpc::Status build_subscribe_notification(gnmi::Notification *notification,
        const gnmi::SubscriptionList &request, bool *sample = nullptr);

    /**
     * @brief Build a notification from the sysrepo changes of a module.
     *
     * @param[out] notification Notification to fill.
     * @param[in] request Subscription list to answer to.
     * @param[in] xpath XPath of the subscription the change notification fires for.
     * @param[in] origin Normalized origin the subscription was made under.
     * @param[in] session Sysrepo session to read the changes from.
     * @return gRPC status.
     */
    grpc::Status build_subscribe_notification_for_changes(gnmi::Notification *notification,
        const gnmi::SubscriptionList &request, std::string &xpath, std::string origin,
        sysrepo::Session session);

    /**
     * @brief Write a response to the stream with synchronized write access.
     *
     * @param[in,out] stream Stream to write the response to.
     * @param[in] response Response to write.
     */
    void write(grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        gnmi::SubscribeResponse response);

  private:
    /**
     * @brief Build the updates of a notification from the data of one path.
     *
     * @param[out] update_list Update list to fill.
     * @param[in] fullpath XPath of the data to fetch.
     * @param[in] encoding Encoding requested by the client.
     * @param[in] origin Origin of the requested data.
     * @return gRPC status.
     */
    grpc::Status build_subs_update_(google::protobuf::RepeatedPtrField<gnmi::Update> *update_list,
        std::string fullpath, gnmi::Encoding encoding, std::string origin);

    /**
     * @brief Register an on-change stream subscription with sysrepo.
     *
     * @param[in] request Subscription request the subscription belongs to.
     * @param[in] sub On-change subscription to register.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @param[in,out] scheduler Scheduler the change notifications are queued to.
     * @param[in] sr_sub Sysrepo data subscription to register on.
     * @param[in,out] params_vec Storage keeping the callback parameters alive.
     * @return gRPC status.
     */
    grpc::Status register_stream_on_change_(gnmi::SubscribeRequest &request, gnmi::Subscription sub,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Scheduler &scheduler, std::shared_ptr<DataSubscribe> sr_sub,
        std::vector<SrModuleOnChangeParams> &params_vec);

    /**
     * @brief Handle a SubscribeRequest with STREAM subscription mode.
     *
     * @param[in] context gRPC server context.
     * @param[in] request Subscription request to handle.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @return gRPC status.
     */
    grpc::Status handle_stream_(grpc::ServerContext *context, gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    /**
     * @brief Handle a SubscribeRequest with ONCE subscription mode.
     *
     * @param[in] request Subscription request to handle.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @return gRPC status.
     */
    grpc::Status handle_once_(gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

    /**
     * @brief Handle a SubscribeRequest with POLL subscription mode.
     *
     * @param[in] request Subscription request to handle.
     * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
     * @return gRPC status.
     */
    grpc::Status handle_poll_(gnmi::SubscribeRequest request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream);

  private:
    sysrepo::Session sr_sess_;        // sysrepo session
    std::shared_ptr<Encode> encodef_; // support for json ietf encoding
    const Auth &auth_;                // authentication/authorization data
    std::recursive_mutex stream_mutex_;
};

} // namespace impl
