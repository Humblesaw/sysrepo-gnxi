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
#include "confirm.h"
#include "rpc.h"

static std::atomic<bool> shutting_down;

// cache server contexts for TryCancel on shutting down
static std::set<ServerContext*> server_contexts;
static std::mutex server_context_mutex;

class ServerContextHolder {
  public:
    ServerContextHolder(ServerContext *ctx) : ctx(ctx) {
      const std::lock_guard<std::mutex> lock(server_context_mutex);
      server_contexts.insert(ctx);
    }
    ~ServerContextHolder() {
      const std::lock_guard<std::mutex> lock(server_context_mutex);
      server_contexts.erase(ctx);
    }
  private:
    ServerContext *ctx;
};

void GNMIService::TryCancelAll(void)
{
  const std::lock_guard<std::mutex> lock(server_context_mutex);
  // forbid any new subscriptions by indicating we are shutting down
  shutting_down.store(true);
  for (auto ctx : server_contexts) {
    ctx->TryCancel();
  }
  BOOST_LOG_TRIVIAL(debug) << "Sent cancellation to subscriptions";
}

Status GNMIService::Set(ServerContext *context, const SetRequest *request,
                        SetResponse *response)
{
  (void)context;
  impl::Set rpc(sr_con.sessionStart(sysrepo::Datastore::Running), sr_con.sessionStart(sysrepo::Datastore::Startup), conf_state);

  return rpc.run(request, response);
}

Status GNMIService::Get(ServerContext *context, const GetRequest *request,
                        GetResponse *response)
{
  (void)context;
  impl::Get rpc(sr_con.sessionStart(sysrepo::Datastore::Running));

  return rpc.run(request, response);
}

Status GNMIService::Subscribe(ServerContext *context,
                              ServerReaderWriter<SubscribeResponse, SubscribeRequest> *stream)
{
  ServerContextHolder holder(context);

  // If we are shutting down don't start any new subscriptions
  // as TryCancelAll will not be called after this.
  if (shutting_down.load()) {
    BOOST_LOG_TRIVIAL(debug) << "Subscribe is not possible as server is shutting down";
    return Status(StatusCode::UNAVAILABLE, string("Server is shutting down"));
  }

  SubscribeRequest request;
  impl::Subscribe rpc(sr_con.sessionStart(sysrepo::Datastore::Running));

  return rpc.run(context, stream);
}

Status GNMIService::Confirm(ServerContext *context, const ConfirmRequest *request,
                            ConfirmResponse *response)
{
  (void)context;
  impl::Confirm rpc(sr_con.sessionStart(sysrepo::Datastore::Startup), conf_state);

  return rpc.run(request, response);
}

Status GNMIService::Rpc(ServerContext *context, const RpcRequest *request,
                        RpcResponse *response)
{
  (void)context;
  impl::Rpc rpc(sr_con.sessionStart(sysrepo::Datastore::Running));

  return rpc.run(request, response);
}
