/**
 * @file rpc.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Rpc RPC header
 *
 * @copyright
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

#include <proto/yang_rpc.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "encode/encode.h"

namespace impl
{

class Rpc
{
  public:
    Rpc(sysrepo::Session sess) : sr_sess(sess) { encodef = std::make_shared<Encode>(sr_sess); }
    ~Rpc() {}

    grpc::Status run(const yang_rpc::RpcRequest *req, yang_rpc::RpcResponse *response);

  private:
    sysrepo::Session sr_sess;        // sysrepo session
    std::shared_ptr<Encode> encodef; // support for json ietf encoding
};

} // namespace impl
