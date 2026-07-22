/**
 * @file gnxi.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNXI service implementation
 *
 * @copyright
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

#include "gnxi.h"

#include "rpc.h"

grpc::Status GNXIService::Rpc(grpc::ServerContext *context, const gnxi::RpcRequest *request,
                              gnxi::RpcResponse *response)
{
    (void)context;
    impl::Rpc rpc(sr_con.sessionStart(sysrepo::Datastore::Running));
    return rpc.run(request, response);
}
