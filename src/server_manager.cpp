/**
 * @file server_manager.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Server manager implementation
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

#include "server_manager.h"

#include <cassert>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

#include <grpcpp/server_builder.h>
#include <sysrepo-cpp/Changes.hpp>

#include "security/auth.h"
#include "utils/log.h"

/**
 * @brief Transport selection of a listen endpoint.
 *
 */
struct EndpointTransport
{
    // the selected transport case, required by the schema
    std::optional<std::string> kind;
    // transport address (ip, dns) or the unix socket path
    std::string address;
    // transport port (ip, dns)
    std::string port;
};

/**
 * @brief Read the transport selection of one listen endpoint node.
 *
 * @param[in] endpoint Listen endpoint node.
 * @return Transport selection.
 */
static EndpointTransport transport_of(const libyang::DataNode &endpoint)
{
    EndpointTransport transport;

    if (auto path = endpoint.findPath("unix/path"))
    {
        transport.kind = "unix";
        transport.address = path->asTerm().valueStr();
    }
    else if (auto address = endpoint.findPath("dns/address"))
    {
        transport.kind = "dns";
        transport.address = address->asTerm().valueStr();
        if (auto port = endpoint.findPath("dns/port"))
        {
            transport.port = port->asTerm().valueStr();
        }
    }
    else if (auto address = endpoint.findPath("ip/address"))
    {
        transport.kind = "ip";
        transport.address = address->asTerm().valueStr();
        if (auto port = endpoint.findPath("ip/port"))
        {
            transport.port = port->asTerm().valueStr();
        }
    }

    return transport;
}

/**
 * @brief Build the gRPC listen URI of an endpoint transport.
 *
 * @param[in] transport Transport selection, validated by sysrepo.
 * @return Listen URI.
 */
static std::string uri_of(const EndpointTransport &transport)
{
    assert(transport.kind.has_value());
    if (transport.kind == "unix")
    {
        return "unix:" + transport.address;
    }
    else if (transport.kind == "dns")
    {
        return "dns:///" + transport.address + ":" + transport.port;
    }

    assert(transport.kind == "ip");
    if (transport.address.find(':') != std::string::npos)
    {
        // IPv6 addresses must be enclosed in brackets
        return "[" + transport.address + "]:" + transport.port;
    }
    return transport.address + ":" + transport.port;
}

/**
 * @brief Load the listen endpoints and build gRPC listen URIs. Every endpoint
 * has a transport and there is at least one of them, enforced by sysrepo.
 *
 * @param[in] sess Sysrepo session.
 * @return Listen endpoint name to listen URI map.
 */
static std::map<std::string, std::string> load_endpoints(sysrepo::Session &sess)
{
    std::map<std::string, std::string> endpoints;

    auto data = sess.getData("/sysrepo-gnxi-server:server/listen/endpoint");
    if (!data.has_value())
    {
        return endpoints;
    }

    for (auto endpoint : data->findXPath("/sysrepo-gnxi-server:server/listen/endpoint"))
    {
        auto name = endpoint.findPath("name")->asTerm().valueStr();
        endpoints.emplace(name, uri_of(transport_of(endpoint)));
    }

    return endpoints;
}

ServerManager::ServerManager(sysrepo::Connection conn, std::shared_ptr<Auth> auth,
    std::shared_ptr<grpc::ServerCredentials> creds)
    : conn_(std::move(conn)), gnmi_(conn_, auth), yang_rpc_(conn_, auth), creds_(std::move(creds))
{
    // the subscription handles subsequent changes only, the initial
    // endpoints are started by start()
    listen_sub_.emplace(conn_.sessionStart(sysrepo::Datastore::Running));
    listen_sub_->data_change_subscribe(
        [this](sysrepo::Session session, uint32_t sub_id, std::string_view module_name,
            std::optional<std::string_view> xpath, sysrepo::Event event, uint32_t request_id)
        {
            (void)sub_id;
            (void)module_name;
            (void)xpath;
            (void)request_id;
            return on_listen_change(session, event);
        },
        "/sysrepo-gnxi-server:server/listen", 0, sysrepo::SubscribeOptions::Default);
}

ServerManager::~ServerManager()
{
    shutdown();
}

void ServerManager::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto sess = conn_.sessionStart(sysrepo::Datastore::Running);
    auto endpoints = load_endpoints(sess);
    assert(!endpoints.empty());

    for (const auto &[name, uri] : endpoints)
    {
        if (!start_endpoint(name, uri))
        {
            // fatal at startup, the operator must fix the configuration
            throw std::runtime_error("failed to listen on '" + uri + "' (endpoint '" + name + "')");
        }
    }
}

bool ServerManager::reconfigure(const std::map<std::string, std::string> &endpoints)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_)
    {
        return true;
    }

    // the servers matching the new configuration are kept
    std::map<std::string, Endpoint> kept;
    std::set<std::string> reused;
    for (const auto &[name, uri] : endpoints)
    {
        auto current = servers_.find(name);
        if (current != servers_.end() && current->second.uri == uri)
        {
            kept.emplace(name, std::move(current->second));
            reused.insert(name);
            continue;
        }

        // an endpoint renamed without changing its address keeps its server
        for (auto &[current_name, entry] : servers_)
        {
            if (reused.count(current_name) != 0 || entry.uri != uri)
            {
                continue;
            }
            kept.emplace(name, std::move(entry));
            reused.insert(current_name);
            SLOG_INFO("Listen endpoint '", current_name, "' renamed to '", name, "'");
            break;
        }
    }

    // the servers not matching the configuration anymore are drained
    for (auto &[name, entry] : servers_)
    {
        if (reused.count(name) == 0)
        {
            drain_endpoint(std::move(entry));
        }
    }
    servers_ = std::move(kept);

    // the endpoints that have no server get one
    for (const auto &[name, uri] : endpoints)
    {
        if (servers_.find(name) == servers_.end() && !start_endpoint(name, uri))
        {
            return false;
        }
    }
    return true;
}

void ServerManager::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_)
    {
        return;
    }
    shutting_down_ = true;

    // end the subscriptions on all the servers, including the ones being
    // drained, so that their blocking Shutdown() calls can return
    GNMIService::try_cancel_all();

    // the deadline gives the RPC handlers a short grace
    for (auto &[name, entry] : servers_)
    {
        (void)name;
        if (entry.server != nullptr)
        {
            entry.server->Shutdown(
                std::chrono::system_clock::now() + std::chrono::milliseconds(100));
        }
    }

    for (auto &drain : draining_)
    {
        if (drain->thread.joinable())
        {
            drain->thread.join();
        }
    }
}

bool ServerManager::start_endpoint(const std::string &name, const std::string &uri)
{
    // mutex_ must be held
    grpc::ServerBuilder builder;
    builder.AddListeningPort(uri, creds_);
    builder.RegisterService(&gnmi_);
    builder.RegisterService(&yang_rpc_);

    auto server = builder.BuildAndStart();
    if (server == nullptr)
    {
        SLOG_ERROR("Failed to listen on '", uri, "' (endpoint '", name, "')");
        return false;
    }

    SLOG_INFO("Listening on '", uri, "' (endpoint '", name, "')");
    servers_[name] = Endpoint{uri, std::move(server)};
    return true;
}

void ServerManager::drain_endpoint(Endpoint &&endpoint)
{
    // mutex_ must be held and the endpoint must have a server
    SLOG_INFO("Draining the listen endpoint on '", endpoint.uri, "'");
    auto uri = endpoint.uri;
    auto server = std::move(endpoint.server);

    auto drain = std::make_unique<Draining>();
    drain->thread = std::thread(
        [uri = std::move(uri), server = std::move(server)]() mutable
        {
            // the listening ports are closed immediately, the in-flight RPCs
            // are allowed to complete
            server->Shutdown();
            SLOG_INFO("Listen endpoint on '", uri, "' drained");
        });
    draining_.push_back(std::move(drain));
}

std::map<std::string, std::string> ServerManager::current_endpoints()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> endpoints;
    for (const auto &[name, entry] : servers_)
    {
        endpoints.emplace(name, entry.uri);
    }
    return endpoints;
}

sysrepo::ErrorCode ServerManager::on_listen_change(sysrepo::Session session, sysrepo::Event event)
{
    switch (event)
    {
    case sysrepo::Event::Change:
        try
        {
            auto preimage = current_endpoints();
            preimage_.reset();

            auto endpoints = load_endpoints(session);
            // YANG min-elements guarantees an endpoint in a valid configuration
            assert(!endpoints.empty());
            if (!reconfigure(endpoints))
            {
                // a failed change must be reverted by this callback itself, it
                // gets no abort event of its own
                if (!reconfigure(preimage))
                {
                    SLOG_ERROR("Failed to revert the listen endpoints, the server is out of sync "
                               "with the datastore");
                }
                session.setErrorMessage("failed to start a listen endpoint server "
                                        "(see the server log for details)");
                return sysrepo::ErrorCode::CallbackFailed;
            }
            preimage_ = std::move(preimage);
        }
        catch (const std::exception &exc)
        {
            session.setErrorMessage(exc.what());
            return sysrepo::ErrorCode::CallbackFailed;
        }
        return sysrepo::ErrorCode::Ok;
    case sysrepo::Event::Done:
        preimage_.reset();
        return sysrepo::ErrorCode::Ok;
    case sysrepo::Event::Abort:
        // the commit was denied by another subscriber, restore the
        // endpoints as they were before it
        if (preimage_.has_value())
        {
            if (!reconfigure(*preimage_))
            {
                SLOG_ERROR("Failed to revert the listen endpoints after an aborted commit, "
                           "the server is out of sync with the datastore");
            }
            preimage_.reset();
        }
        return sysrepo::ErrorCode::Ok;
    default:
        return sysrepo::ErrorCode::Ok;
    }
}
