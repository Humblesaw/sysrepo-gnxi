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

#include "gnmi.h"

#include "get.h"
#include "set.h"
#include "subscribe.h"
#include "utils/log.h"

static std::atomic<bool> shutting_down;

// cache server contexts for TryCancel on shutting down
static std::set<grpc::ServerContext *> server_contexts;
static std::mutex server_context_mutex;

class ServerContextHolder
{
  public:
    ServerContextHolder(grpc::ServerContext *ctx) : ctx(ctx)
    {
        const std::lock_guard<std::mutex> lock(server_context_mutex);
        server_contexts.insert(ctx);
    }
    ~ServerContextHolder()
    {
        const std::lock_guard<std::mutex> lock(server_context_mutex);
        server_contexts.erase(ctx);
    }

  private:
    grpc::ServerContext *ctx;
};

void GNMIService::TryCancelAll(void)
{
    const std::lock_guard<std::mutex> lock(server_context_mutex);
    // forbid any new subscriptions by indicating we are shutting down
    shutting_down.store(true);
    for (auto ctx : server_contexts)
    {
        ctx->TryCancel();
    }
    SLOG_DEBUG("Sent cancellation to subscriptions");
}

grpc::Status GNMIService::Set(grpc::ServerContext *context, const gnmi::SetRequest *request,
                              gnmi::SetResponse *response)
{
    (void)context;
    impl::Set rpc(sr_con.sessionStart(sysrepo::Datastore::Startup),
                  sr_con.sessionStart(sysrepo::Datastore::Running),
                  sr_con.sessionStart(sysrepo::Datastore::Candidate), commit_state);
    return rpc.run(request, response);
}

grpc::Status GNMIService::Get(grpc::ServerContext *context, const gnmi::GetRequest *request,
                              gnmi::GetResponse *response)
{
    (void)context;
    impl::Get rpc(sr_con.sessionStart(sysrepo::Datastore::Running));
    return rpc.run(request, response);
}

grpc::Status GNMIService::Subscribe(
    grpc::ServerContext *context,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    ServerContextHolder holder(context);

    // If we are shutting down don't start any new subscriptions
    // as TryCancelAll will not be called after this.
    if (shutting_down.load())
    {
        SLOG_DEBUG("Subscribe is not possible as server is shutting down");
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string("Server is shutting down"));
    }

    gnmi::SubscribeRequest request;
    impl::Subscribe rpc(sr_con.sessionStart(sysrepo::Datastore::Running));
    return rpc.run(context, stream);
}
