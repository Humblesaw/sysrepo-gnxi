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

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include <grpc/status.h>

#include "confirm.h"
#include "encode/encode.h"
#include "utils/sysrepo.h"

namespace impl
{

class Set
{
  public:
    Set(sysrepo::Session startup_sess, sysrepo::Session running_sess,
        sysrepo::Session candidate_sess, std::shared_ptr<ConfirmState> confirm_state)
        : sr_sess_startup(startup_sess), sr_sess(running_sess), sr_sess_candidate(candidate_sess),
          conf_state(confirm_state)
    {
        encodef = std::make_shared<Encode>(sr_sess);
    }
    ~Set() {}

    grpc::Status run(const gnmi::SetRequest *request, gnmi::SetResponse *response);

  private:
    grpc::Status handleUpdate(gnmi::Update in, gnmi::UpdateResult *out, std::string prefix_str,
                              const gnmi::Path &prefix, std::string op);

  private:
    sysrepo::Session sr_sess_startup;         // sysrepo startup datastore session
    sysrepo::Session sr_sess;                 // sysrepo running datastore session
    sysrepo::Session sr_sess_candidate;       // sysrepo candidate datastore session
    std::shared_ptr<Encode> encodef;          // support for json ietf encoding
    std::shared_ptr<ConfirmState> conf_state; // commit confirm state
    std::optional<libyang::DataNode> deleteTree, purgeTree, replaceTree, updateTree;
    UpdateTransaction xact;
};

} // namespace impl
