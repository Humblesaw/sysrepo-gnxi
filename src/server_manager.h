/**
 * @file server_manager.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Server manager header
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

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/server.h>

#include <sysrepo-cpp/Connection.hpp>

#include "gnmi/gnmi.h"
#include "utils/sysrepo.h"
#include "yang_rpc/yang_rpc.h"

class Auth;

/**
 * @brief One gRPC server per configured listen endpoint, reconciled with
 * sysrepo.
 *
 * A gRPC server cannot add or remove listening ports once it is built and
 * started, so every listen endpoint of
 * sysrepo-gnxi-server:server/listen maps to its own grpc::Server, all of
 * them registering the same services and sharing the same credentials.
 *
 * Draining closes the listening ports immediately (new connections are
 * refused) but lets all in-flight RPCs run to completion, an
 * established connection is never terminated by a configuration change,
 * only by its client. The drain runs on its own thread because
 * grpc::Server::Shutdown() without a deadline blocks until the last
 * in-flight RPC finishes.
 */
class ServerManager
{
  public:
    /**
     * @brief Create the manager and subscribe to the listen endpoint
     * changes.
     *
     * @param[in] conn Sysrepo connection.
     * @param[in] auth Authentication providing the services with per-RPC sessions.
     * @param[in] creds Credentials shared by all the servers.
     */
    ServerManager(sysrepo::Connection conn, std::shared_ptr<Auth> auth,
        std::shared_ptr<grpc::ServerCredentials> creds);

    /**
     * @brief Shut down all the servers and join the drain threads.
     */
    ~ServerManager();

    ServerManager(const ServerManager &) = delete;
    ServerManager &operator=(const ServerManager &) = delete;
    ServerManager(ServerManager &&) = delete;
    ServerManager &operator=(ServerManager &&) = delete;

    /**
     * @brief Load the listen endpoints from sysrepo and start a server for
     * each. Throw when a server cannot listen on its address.
     *
     * A non-empty endpoint list is guaranteed by the YANG model.
     */
    void start();

    /**
     * @brief Reconcile the running servers with the listen endpoints.
     *
     * @param[in] endpoints Listen endpoint name to listen URI map.
     * @return True when all the endpoints are served, false when a server
     * failed to start.
     */
    bool reconfigure(const std::map<std::string, std::string> &endpoints);

    /**
     * @brief Shut down all the servers (the in-flight RPCs get a 100 ms
     * grace) and join the drain threads.
     */
    void shutdown();

  private:
    /**
     * @brief A listen endpoint and its running server.
     */
    struct Endpoint
    {
        std::string uri;
        std::unique_ptr<grpc::Server> server;
    };

    /**
     * @brief A server being drained on its own thread.
     */
    struct Draining
    {
        std::thread thread;
    };

    /**
     * @brief Start the server of an endpoint, mutex_ must be held.
     *
     * @param[in] name Listen endpoint name.
     * @param[in] uri Listen URI.
     * @return True when listening.
     */
    bool start_endpoint(const std::string &name, const std::string &uri);

    /**
     * @brief Drain the server of an endpoint on its own thread, mutex_
     * must be held and the endpoint must have a server.
     *
     * @param[in] endpoint Endpoint to drain.
     */
    void drain_endpoint(Endpoint &&endpoint);

    /**
     * @brief The endpoints currently served, without any change being applied.
     *
     * @return Current listen endpoint name to listen URI map.
     */
    std::map<std::string, std::string> current_endpoints();

    /**
     * @brief Reconcile the servers with the listen endpoints on a change
     * and revert them on abort.
     *
     * @param[in] session Callback session.
     * @param[in] event Callback event.
     * @return Ok, or a failure rejecting the change.
     */
    sysrepo::ErrorCode on_listen_change(sysrepo::Session session, sysrepo::Event event);

    sysrepo::Connection conn_;
    GNMIService gnmi_;
    YangRpcService yang_rpc_;
    std::shared_ptr<grpc::ServerCredentials> creds_;
    std::optional<DataSubscribe> listen_sub_;
    std::mutex mutex_;
    std::map<std::string, Endpoint> servers_;
    std::vector<std::unique_ptr<Draining>> draining_;
    bool shutting_down_ = false;
    // the endpoints before the change being applied, for a revert on abort
    std::optional<std::map<std::string, std::string>> preimage_;
};
