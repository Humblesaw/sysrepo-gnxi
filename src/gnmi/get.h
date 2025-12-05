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

#pragma once

#include "encode/encode.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

namespace impl
{

class Get
{
  public:
    Get(sysrepo::Session sess) : sr_sess(sess) { encodef = std::make_shared<Encode>(sr_sess); }
    ~Get() {}

    grpc::Status run(const gnmi::GetRequest *req, gnmi::GetResponse *response);

  private:
    grpc::Status BuildGetNotification(gnmi::Notification *notification, const gnmi::Path &prefix,
                                      const gnmi::Path &path, gnmi::Encoding encoding,
                                      gnmi::GetRequest_DataType dataType);
    grpc::Status BuildGetUpdate(google::protobuf::RepeatedPtrField<gnmi::Update> *updateList,
                                const std::string &fullpath, gnmi::Encoding encoding);

  private:
    sysrepo::Session sr_sess;        // sysrepo session
    std::shared_ptr<Encode> encodef; // support for json ietf encoding
};

} // namespace impl
