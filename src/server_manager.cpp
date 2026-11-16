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
#include <filesystem>
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
 * @brief Percent-encode a literal Unix socket path for a gRPC URI.
 *
 * @param[in] path Literal socket path.
 * @return Percent-encoded path.
 * @throws std::invalid_argument on an embedded NUL byte.
 */
static std::string encode_unix_path(const std::string &path)
{
    if (path.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("unix listen path contains an embedded NUL byte");
    }

    static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const auto byte : path)
    {
        const auto ch = static_cast<unsigned char>(byte);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/')
        {
            encoded += byte;
        }
        else
        {
            encoded += '%';
            encoded += HEX_DIGITS[ch >> 4];
            encoded += HEX_DIGITS[ch & 0x0f];
        }
    }
    return encoded;
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
        return "unix:" + encode_unix_path(transport.address);
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
 * @return Listen endpoint name to address map, including literal Unix paths.
 */
static std::map<std::string, ServerManager::EndpointAddress> load_endpoints(sysrepo::Session &sess)
{
    std::map<std::string, ServerManager::EndpointAddress> endpoints;

    auto data = sess.getData("/sysrepo-gnxi-server:server/listen/endpoint");
    if (!data.has_value())
    {
        return endpoints;
    }

    for (auto endpoint : data->findXPath("/sysrepo-gnxi-server:server/listen/endpoint"))
    {
        auto name = endpoint.findPath("name")->asTerm().valueStr();
        auto transport = transport_of(endpoint);
        endpoints.emplace(name, ServerManager::EndpointAddress{uri_of(transport),
                                    transport.kind == "unix" ? transport.address : ""});
    }

    return endpoints;
}

ServerManager::ServerManager(sysrepo::Connection conn, std::shared_ptr<Auth> auth,
    std::shared_ptr<grpc::ServerCredentials> creds)
    : conn_(std::move(conn)), gnmi_(conn_, auth), yang_rpc_(conn_, auth), creds_(std::move(creds))
{
}

ServerManager::~ServerManager()
{
    shutdown();

    // unsubscribe before the members the callbacks use are destroyed
    listen_sub_.reset();
}

void ServerManager::start()
{
    // keep separated from constructor so that destructor is called when setup throws
    listen_sub_.emplace(conn_.sessionStart(sysrepo::Datastore::Running));
    listen_sub_->data_change_subscribe(
        [this](sysrepo::Session session, uint32_t sub_id, std::string_view module_name,
            std::optional<std::string_view> xpath, sysrepo::Event event, uint32_t request_id)
        {
            (void)sub_id;
            (void)module_name;
            (void)xpath;
            (void)request_id;
            return on_listen_change_(session, event);
        },
        "/sysrepo-gnxi-server:server/listen", 0, sysrepo::SubscribeOptions::Enabled);
}

void ServerManager::reconfigure(const std::map<std::string, EndpointAddress> &endpoints)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_)
    {
        return;
    }

    // gRPC unlinks a unix socket path when a replacement binds or drains, so
    // independent servers cannot safely share one pathname
    std::set<std::string> unix_uris;
    for (const auto &[name, address] : endpoints)
    {
        (void)name;
        if (address.uri_.starts_with("unix:") && !unix_uris.insert(address.uri_).second)
        {
            throw std::invalid_argument("duplicate unix listen URI '" + address.uri_ + "'");
        }
    }

    reap_drains_();

    // old servers to keep without changing the running set yet
    // map each final endpoint name to its current one
    // server can be reused by only one endpoint, even when endpoints share a URI
    std::map<std::string, std::string> kept_servers;
    std::set<std::string> reused_uris;
    for (const auto &[name, address] : endpoints)
    {
        // unchanged/renamed endpoint without changing its URI keeps its server
        for (auto match = servers_.begin(); match != servers_.end(); ++match)
        {
            if (reused_uris.contains(match->first) || match->second.address_.uri_ != address.uri_)
            {
                continue;
            }
            kept_servers.emplace(name, match->first);
            reused_uris.insert(match->first);
            break;
        }
    }

    // start the new servers before draining any old ones, the running set is
    // left untouched if any of them fails
    std::map<std::string, Endpoint> started;
    try
    {
        for (const auto &[name, address] : endpoints)
        {
            if (kept_servers.contains(name))
            {
                continue;
            }
            auto endpoint = start_endpoint_(name, address);
            if (!endpoint.has_value())
            {
                throw std::runtime_error("failed to start a listen endpoint server "
                                         "(see the server log for details)");
            }
            started.emplace(name, std::move(endpoint.value()));
        }
    }
    catch (...)
    {
        for (auto &[name, entry] : started)
        {
            (void)name;
            drain_endpoint_(std::move(entry));
        }
        throw;
    }

    // move the servers beside the newly started ones
    for (const auto &[name, old_name] : kept_servers)
    {
        if (name != old_name)
        {
            SLOG_INFO("Listen endpoint '", old_name, "' renamed to '", name, "'");
        }
        auto entry = servers_.extract(old_name);
        started.emplace(name, std::move(entry.mapped()));
    }

    // drain the obsolete servers
    for (auto &[name, entry] : servers_)
    {
        drain_endpoint_(std::move(entry));
    }
    servers_ = std::move(started);
}

void ServerManager::shutdown()
{
    std::map<std::string, Endpoint> servers;
    std::vector<std::unique_ptr<Draining>> draining;
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
        servers = std::move(servers_);
        draining = std::move(draining_);
    }

    // the deadline cancels calls without a running handler, any handlers
    // already running can still delay shutdown beyond it
    for (auto &[name, entry] : servers)
    {
        (void)name;
        if (entry.server_ != nullptr)
        {
            entry.server_->Shutdown(
                std::chrono::system_clock::now() + std::chrono::milliseconds(100));
        }
    }

    for (auto &drain : draining)
    {
        if (drain->thread_.joinable())
        {
            drain->thread_.join();
        }
    }
}

bool ServerManager::wait_for_unix_rebind_(const EndpointAddress &address) const
{
    // mutex_ must be held
    if (!address.uri_.starts_with("unix:"))
    {
        return true;
    }

    // is the server being drained?
    bool being_drained = false;
    for (const auto &drain : draining_)
    {
        if (drain->uri_ == address.uri_ && !drain->finished_.load())
        {
            being_drained = true;
            break;
        }
    }
    if (!being_drained)
    {
        return true;
    }

    // the server is being drained, check whether the socket path exists
    // if we bind and it still exists, the drain removes it afterwards (race condition)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (
        std::filesystem::exists(address.unix_path_) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !std::filesystem::exists(address.unix_path_);
}

std::optional<ServerManager::Endpoint> ServerManager::start_endpoint_(
    const std::string &name, const EndpointAddress &address)
{
    // mutex_ must be held
    const auto &uri = address.uri_;
    if (!wait_for_unix_rebind_(address))
    {
        SLOG_ERROR("Failed to listen on '", uri, "' (endpoint '", name,
            "'): the draining server did not release its unix socket path");
        return std::nullopt;
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort(uri, creds_);
    builder.RegisterService(&gnmi_);
    builder.RegisterService(&yang_rpc_);

    auto server = builder.BuildAndStart();
    if (server == nullptr)
    {
        SLOG_ERROR("Failed to listen on '", uri, "' (endpoint '", name, "')");
        return std::nullopt;
    }

    SLOG_INFO("Listening on '", uri, "' (endpoint '", name, "')");
    return Endpoint{address, std::move(server)};
}

void ServerManager::drain_endpoint_(Endpoint &&endpoint)
{
    // mutex_ must be held and the endpoint must have a server
    SLOG_INFO("Draining the listen endpoint on '", endpoint.address_.uri_, "'");
    auto uri = endpoint.address_.uri_;
    auto server = std::move(endpoint.server_);

    auto drain = std::make_unique<Draining>();
    drain->uri_ = uri;
    auto *state = drain.get();
    drain->thread_ = std::thread(
        [uri = std::move(uri), server = std::move(server), state]() mutable
        {
            // the deadline cancels calls without a running handler, while an
            // already-running handler must still finish its work
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
            state->finished_.store(true);
            SLOG_INFO("Listen endpoint on '", uri, "' drained");
        });
    draining_.push_back(std::move(drain));
}

void ServerManager::reap_drains_()
{
    // mutex_ must be held
    for (auto it = draining_.begin(); it != draining_.end();)
    {
        if (!(*it)->finished_.load())
        {
            it++;
            continue;
        }

        (*it)->thread_.join();
        it = draining_.erase(it);
    }
}

void ServerManager::drain_uncommitted_(const std::map<std::string, EndpointAddress> &committed)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<std::string> retained;
    for (const auto &endpoint : committed)
    {
        for (const auto &[name, server] : servers_)
        {
            // retain the server if it has not been retained yet and has unchanged URI
            if (!retained.contains(name) && server.address_.uri_ == endpoint.second.uri_)
            {
                retained.insert(name);
                break;
            }
        }
    }

    // drain all servers which are uncommitted
    for (auto it = servers_.begin(); it != servers_.end();)
    {
        if (retained.contains(it->first))
        {
            ++it;
            continue;
        }
        drain_endpoint_(std::move(it->second));
        it = servers_.erase(it);
    }
}

std::map<std::string, ServerManager::EndpointAddress> ServerManager::current_endpoints_()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, EndpointAddress> endpoints;
    for (const auto &[name, entry] : servers_)
    {
        endpoints.emplace(name, entry.address_);
    }
    return endpoints;
}

sysrepo::ErrorCode ServerManager::on_listen_change_(sysrepo::Session session, sysrepo::Event event)
{
    switch (event)
    {
    case sysrepo::Event::Enabled:
    case sysrepo::Event::Change:
        try
        {
            preimage_ = current_endpoints_();
            auto endpoints = load_endpoints(session);
            // YANG min-elements guarantees an endpoint in a valid configuration
            assert(!endpoints.empty());
            reconfigure(endpoints);
        }
        catch (const std::exception &exc)
        {
            // a failed change must be reverted by this callback itself, it
            // gets no abort event of its own
            if (preimage_.has_value())
            {
                try
                {
                    reconfigure(*preimage_);
                }
                catch (const std::exception &rollback_exc)
                {
                    drain_uncommitted_(*preimage_);
                    SLOG_ERROR("Failed to revert the listen endpoints: ", rollback_exc.what(),
                        " - uncommitted listeners were drained");
                }
                preimage_.reset();
            }
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
            try
            {
                reconfigure(*preimage_);
            }
            catch (const std::exception &rollback_exc)
            {
                drain_uncommitted_(*preimage_);
                SLOG_ERROR("Failed to revert the listen endpoints: ", rollback_exc.what(),
                    " - uncommitted listeners were drained");
            }
            preimage_.reset();
        }
        return sysrepo::ErrorCode::Ok;
    default:
        return sysrepo::ErrorCode::Ok;
    }
}
