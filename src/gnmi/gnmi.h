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

#ifndef _GNMI_SERVER_H
#define _GNMI_SERVER_H

#include <future>

#include <proto/gnmi.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/utils/exception.hpp>

#include "encode/encode.h"
#include "confirm.h"
#include "utils/log.h"

using namespace grpc;
using namespace gnmi;

using sysrepo::Session;
using sysrepo::Connection;
using google::protobuf::RepeatedPtrField;
using std::make_shared;

class GNMIService final : public gNMI::Service
{
  public:
    GNMIService(sysrepo::Connection conn) : sr_con(conn) {
      conf_state = make_shared<impl::ConfirmState>(conn);
    }
    ~GNMIService() {BOOST_LOG_TRIVIAL(info) << "Quitting GNMI Server"; }

    Status Capabilities(ServerContext* context,
        const CapabilityRequest* request, CapabilityResponse* response);

    Status Get(ServerContext* context,
        const GetRequest* request, GetResponse* response);

    Status Set(ServerContext* context,
        const SetRequest* request, SetResponse* response);

    Status Subscribe(ServerContext* context,
        ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

    Status Confirm(ServerContext *context,
        const ConfirmRequest *request, ConfirmResponse *response);

    Status Rpc(ServerContext *context,
        const RpcRequest *request, RpcResponse *response);

    static void TryCancelAll(void);

  private:
    void ServerContextUpdate(ServerContext *ctx, bool add);
    sysrepo::Connection sr_con; //sysrepo connection
    shared_ptr<impl::ConfirmState> conf_state;
};

void RunServer(string bind_addr, shared_ptr<ServerCredentials> cred, sysrepo::Connection sr_conn, std::promise<void> ready = std::promise<void>());

void SetupSignalHandler(bool daemon = true);

#endif //_GNMI_SERVER_H
