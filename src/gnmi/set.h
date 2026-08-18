/**
 * @file set.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Set RPC header
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

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include <grpc/status.h>

#include "commit.h"
#include "encode/encode.h"
#include "security/auth.h"
#include "utils/sysrepo.h"

namespace impl
{

class Set
{
  public:
    Set(sysrepo::Session running_sess, std::shared_ptr<Commit> commit_state, const Auth &auth)
        : sr_sess(running_sess), commit_state(commit_state), auth_(auth)
    {
        encodef = std::make_shared<Encode>(sr_sess);
    }
    ~Set() {}

    grpc::Status run(grpc::ServerContext *context, const gnmi::SetRequest *request,
                     gnmi::SetResponse *response);

  private:
    grpc::Status handleUpdate(gnmi::Update in, gnmi::UpdateResult *out, std::string prefix_str,
                              const gnmi::Path &prefix, std::string op);

  private:
    sysrepo::Session sr_sess;             // sysrepo running datastore session
    std::shared_ptr<Encode> encodef;      // support for json ietf encoding
    std::shared_ptr<Commit> commit_state; // commit confirm state
    const Auth &auth_;                    // authentication/authorization data
    std::optional<libyang::DataNode> deleteTree, purgeTree, replaceTree, updateTree;
    UpdateTransaction xact;
};

} // namespace impl
