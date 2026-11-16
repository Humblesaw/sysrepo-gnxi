/**
 * @file credentials.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief TLS server credentials header
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

#include <cstdint>
#include <memory>
#include <optional>

#include <grpc/grpc_security.h>
#include <grpcpp/security/server_credentials.h>

#include <sysrepo-cpp/Connection.hpp>

#include "tls.h"
#include "utils/sysrepo.h"

struct TlsState;

/**
 * @brief gRPC server credentials with the TLS material reloadable from
 * sysrepo.
 *
 * The credentials are built on the gRPC C core certificate config fetcher,
 * which gRPC queries every time a new connection is established, so a
 * material change applies to the new connections without restarting the
 * server, while the established connections keep the material they were
 * established with. Changes of /ietf-keystore:keystore,
 * /ietf-truststore:truststore and /sysrepo-gnxi-server:server/tls are
 * subscribed to and reloaded into an in-memory cache the fetcher reads
 * from.
 */
class ReloadableCredentials
{
  public:
    /**
     * @brief Create the manager without starting subscriptions.
     *
     * @param[in] conn Sysrepo connection.
     */
    explicit ReloadableCredentials(sysrepo::Connection conn);

    /**
     * @brief Subscribe to material changes and create the credentials.
     *
     * Kept separate from the constructor so a failure at any subsequent stage
     * runs the actual destructor before callbacks or members are disposed.
     */
    void start();

    /**
     * @brief Unsubscribe from the material changes.
     */
    ~ReloadableCredentials();

    ReloadableCredentials(const ReloadableCredentials &) = delete;
    ReloadableCredentials &operator=(const ReloadableCredentials &) = delete;
    ReloadableCredentials(ReloadableCredentials &&) = delete;
    ReloadableCredentials &operator=(ReloadableCredentials &&) = delete;

    /**
     * @brief Get the gRPC server credentials.
     *
     * @return gRPC server credentials.
     */
    std::shared_ptr<grpc::ServerCredentials> credentials() const;

  private:
    /**
     * @brief Certificate config fetch callback of the gRPC C core.
     *
     * @param[in] user_data Shared TLS material state.
     * @param[out] config Certificate config for gRPC to take the ownership of.
     * @return Reload status of the fetch.
     */
    static grpc_ssl_certificate_config_reload_status fetch_config_(
        void *user_data, grpc_ssl_server_certificate_config **config);

    /**
     * @brief Validate the candidate material on a change, reload it on
     * commit.
     *
     * @param[in] session Callback session.
     * @param[in] event Callback event.
     * @param[in] request_id Operation ID shared by all module callbacks of a
     * transaction, used to validate and apply each of them only once.
     * @return Ok, or a failure rejecting a change gRPC cannot serve.
     */
    sysrepo::ErrorCode on_material_change_(
        sysrepo::Session session, sysrepo::Event event, uint32_t request_id);

    sysrepo::Connection conn_;

    // the material and the fetch callback are synchronized by this shared state
    std::shared_ptr<TlsState> state_;
    std::optional<DataSubscribe> subscription_;
    std::shared_ptr<grpc::ServerCredentials> credentials_;
    bool initialized_ = false;
    std::optional<uint32_t> validated_operation_;
    std::optional<TlsMaterial> validated_material_;
    std::optional<uint32_t> applied_operation_;
};
