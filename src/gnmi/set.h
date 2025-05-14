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

#ifndef _GNMI_SET_H
#define _GNMI_SET_H

#include <tuple>
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "confirm.h"
#include "encode/encode.h"

using namespace gnmi;
using grpc::Status;
using grpc::StatusCode;

namespace impl {

class Set {
  public:
    Set(sysrepo::Session running_sess, sysrepo::Session startup_sess, shared_ptr<ConfirmState> confirm_state)
      : sr_sess(running_sess), sr_sess_startup(startup_sess), conf_state(confirm_state)
    {
      encodef = std::make_shared<Encode>(sr_sess);
    }
    ~Set() {}

    Status run(const SetRequest* request, SetResponse* response);

  private:
    std::tuple<grpc::Status, std::optional<libyang::DataNode>> handleUpdate(Update in, UpdateResult *out, string prefix_str, const Path &prefix, string op);

  private:
    sysrepo::Session sr_sess; //sysrepo running datastore session
    sysrepo::Session sr_sess_startup; //sysrepo startup datastore session
    shared_ptr<Encode> encodef; //support for json ietf encoding
    shared_ptr<ConfirmState> conf_state; // commit confirm state
};

}

#endif //_GNMI_SET_H
