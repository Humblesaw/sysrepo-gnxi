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

#ifndef _GNMI_SUBSCRIBE_H
#define _GNMI_SUBSCRIBE_H

#include <proto/gnmi.grpc.pb.h>
#include <boost/asio.hpp>

#include <sysrepo-cpp/Connection.hpp>
#include "encode/encode.h"
#include "utils/sysrepo.h"

using namespace gnmi;
using google::protobuf::RepeatedPtrField;
using grpc::ServerReaderWriter;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

namespace impl {

class SrModuleOnChangeParams;

class Subscribe {
  public:
    Subscribe(sysrepo::Session sess)
      : sr_sess(sess)
    {
      encodef = std::make_shared<Encode>(sr_sess);
    }
    ~Subscribe() {}

    Status run(ServerContext* context,
               ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

    void streamWorker(ServerContext* context, SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
              boost::asio::io_context &initial_update_io,
              boost::asio::io_context &incr_update_io);
    void triggerSampleUpdate(
        ServerContext* context, Subscription &sub,
        ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);
    Status BuildSubscribeNotification(Notification *notification,
                                      const SubscriptionList& request,
				      bool *sample=nullptr);
    Status BuildSubscribeNotificationForChanges(Notification *notification,
                                                const SubscriptionList& request,
                                                string& xpath,
                                                sysrepo::Session session);
    // To synchronize write access to the stream
    void Write(ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
	       SubscribeResponse response);
    // To synchronize posting a write to the stream
    void PostWrite(ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
	       std::unique_ptr<SubscribeResponse> response, boost::asio::io_context &io);
  private:
    Status BuildSubsUpdate(RepeatedPtrField<Update>* updateList,
                           const Path &prefix, string fullpath,
                           gnmi::Encoding encoding);
    Status registerStreamOnChange(
              SubscribeRequest &request, Subscription sub,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
              boost::asio::io_context &initial_update_io_context,
              boost::asio::io_context &incr_update_io_context,
              shared_ptr<DataSubscribe> sr_sub,
              vector<SrModuleOnChangeParams> &params_vec);
    Status handleStream(ServerContext* context, SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);
    Status handleOnce(SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);
    Status handlePoll(SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

  private:
    sysrepo::Session sr_sess; //sysrepo session
    std::shared_ptr<Encode> encodef; //support for json ietf encoding
    std::recursive_mutex stream_mutex;
};

}

#endif //_GNMI_SUBSCRIBE_H
