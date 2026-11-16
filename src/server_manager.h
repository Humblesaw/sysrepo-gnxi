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

#include <atomic>
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
 * Candidate servers start at the sysrepo Change event, before the transaction
 * commits, and can therefore serve RPCs while other subscribers are still
 * validating it. If the transaction is aborted, these provisional servers are
 * drained like any other removed endpoint, new RPCs are rejected, while RPCs
 * already in flight, including those started before the abort, run to
 * completion.
 *
 * Draining closes the listening ports immediately and invokes gRPC shutdown
 * with a 100 ms deadline on the individual endpoint. Queued calls are
 * cancelled after the deadline, while already-running handlers must still
 * return. Other listeners are unaffected. The drain runs on its own thread
 * because graceful shutdown and handler cleanup may otherwise block.
 */
class ServerManager
{
  public:
    /**
     * @brief An encoded listen URI and its original Unix filesystem path.
     *
     * unix_path_ is empty for non-Unix endpoints.
     */
    struct EndpointAddress
    {
        std::string uri_;
        std::string unix_path_;
    };

    /**
     * @brief Create the manager.
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
     * @brief Subscribe to listen endpoint data and start a server for each
     * endpoint. Throw when a server cannot listen on its address.
     *
     * A non-empty endpoint list is guaranteed by the YANG model.
     */
    void start();

    /**
     * @brief Reconcile the running servers with the listen endpoints.
     *
     * Duplicate Internet-protocol URIs get distinct servers, but duplicate
     * unix URIs are rejected because their socket paths cannot be owned
     * independently.
     *
     * @param[in] endpoints Listen endpoint name to listen URI map.
     */
    void reconfigure(const std::map<std::string, EndpointAddress> &endpoints);

    /**
     * @brief Shut down all the servers and join the drain threads.
     *
     * Calls without a running handler get 100 ms before cancellation,
     * but already-running handlers are waited for rather than forcibly
     * terminated.
     */
    void shutdown();

  private:
    /**
     * @brief A listen endpoint and its running server.
     */
    struct Endpoint
    {
        EndpointAddress address_;
        std::unique_ptr<grpc::Server> server_;
    };

    /**
     * @brief A server being drained on its own thread.
     */
    struct Draining
    {
        std::string uri_;
        std::thread thread_;
        std::atomic<bool> finished_ = false;
    };

    /**
     * @brief Start the server of an endpoint, mutex_ must be held.
     *
     * @param[in] name Listen endpoint name.
     * @param[in] address Listen URI and original Unix path.
     * @return The endpoint with its listening server, or empty on failure.
     */
    std::optional<Endpoint> start_endpoint_(
        const std::string &name, const EndpointAddress &address);

    /**
     * @brief Wait until a draining server releases a reused unix socket path.
     *
     * A drain completes only after all in-flight RPCs, but gRPC unlinks its
     * unix socket path as soon as the listener shuts down. Waiting for the
     * path to disappear prevents the old server from unlinking the path of
     * its replacement. mutex_ must be held.
     *
     * @param[in] address Listen URI and original Unix path to bind.
     * @return True when binding the URI is safe, false on a timeout.
     */
    bool wait_for_unix_rebind_(const EndpointAddress &address) const;

    /**
     * @brief Drain the server of an endpoint on its own thread, mutex_
     * must be held and the endpoint must have a server.
     *
     * @param[in] endpoint Endpoint to drain.
     */
    void drain_endpoint_(Endpoint &&endpoint);

    /**
     * @brief Join and forget finished drained servers, mutex_ must be held.
     */
    void reap_drains_();

    /**
     * @brief Drain every listener absent from a committed endpoint set.
     *
     * Used when reverting a failed transaction cannot restore the complete
     * preimage.
     *
     * @param[in] committed Committed endpoint name to listen URI map.
     */
    void drain_uncommitted_(const std::map<std::string, EndpointAddress> &committed);

    /**
     * @brief The endpoints currently served, without any change being applied.
     *
     * @return Current listen endpoint name to listen URI map.
     */
    std::map<std::string, EndpointAddress> current_endpoints_();

    /**
     * @brief Reconcile the servers with the listen endpoints on a change
     * and revert them on abort.
     *
     * @param[in] session Callback session.
     * @param[in] event Callback event.
     * @return Ok, or a failure rejecting the change.
     */
    sysrepo::ErrorCode on_listen_change_(sysrepo::Session session, sysrepo::Event event);

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
    std::optional<std::map<std::string, EndpointAddress>> preimage_;
};
