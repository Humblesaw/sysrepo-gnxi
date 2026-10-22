/**
 * @file auth.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Authentication/authorization header
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
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

#include <string>

#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_context.h>

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>

/**
 * @brief gRPC server credentials and user authentication.
 *
 * In insecure mode authentication and authorization are disabled, in secure
 * mode mutual TLS is enforced and users are checked against the user
 * database stored in sysrepo. Authorization is delegated to the sysrepo
 * NACM implementation, the authenticated username is pushed into each
 * per-RPC sysrepo session as its NACM user.
 */
class Auth
{
  public:
    /**
     * @brief Server TLS material (the base64 bodies stored in sysrepo wrapped
     * in PEM markers for gRPC)
     *
     */
    struct TlsMaterial
    {
        std::string private_key_pem;
        std::string certificate_pem;
        std::string ca_certificate_pem;
    };

    /**
     * @brief Create the gRPC server credentials.
     *
     * Insecure mode uses no TLS and disables authentication/authorization,
     * whereas secure mode requires complete TLS material.
     *
     * @param[in] insecure Whether to use insecure mode.
     * @param[in] conn Sysrepo connection, ignored in insecure mode.
     * @param[in] tls TLS material, ignored in insecure mode.
     */
    Auth(bool insecure, sysrepo::Connection conn, const TlsMaterial &tls = {});

    Auth(const Auth &) = delete;
    Auth &operator=(const Auth &) = delete;
    Auth(Auth &&) = delete;
    Auth &operator=(Auth &&) = delete;

    /**
     * @brief Get the gRPC server credentials.
     *
     * @return gRPC server credentials.
     */
    std::shared_ptr<grpc::ServerCredentials> credentials() const;

    /**
     * @brief Get the authenticated username from the gRPC auth context.
     *
     * @param[in] ctx Server context to read the username from.
     * @return Authenticated username, or an empty string when unavailable/insecure.
     */
    std::string username(grpc::ServerContext *ctx) const;

    /**
     * @brief Authenticate username and password against known users. Throw on error.
     *
     * @param[in] sess Sysrepo session to read the users from.
     * @param[in] username Username to authenticate.
     * @param[in] password Password to authenticate.
     */
    void authenticate(
        sysrepo::Session &sess, const std::string &username, const std::string &password) const;

    /**
     * @brief Set the authenticated username as the NACM user of a session,
     * enabling NACM enforcement (read filtering, write and RPC denial) for
     * all sysrepo operations done over it. Throw on error.
     *
     * No-op in insecure mode (no NACM user, operations are unrestricted).
     *
     * @param[in,out] sess Session to set the NACM user on.
     * @param[in] ctx Server context to read the authenticated username from.
     */
    void set_nacm_user(sysrepo::Session &sess, grpc::ServerContext *ctx) const;

  private:
    bool insecure_;
    std::shared_ptr<grpc::ServerCredentials> credentials_;
};

/**
 * @brief gRPC metadata processor authenticating username/password call credentials.
 *
 */
class UserPassAuthenticator final : public grpc::AuthMetadataProcessor
{
  public:
    explicit UserPassAuthenticator(const Auth &auth, sysrepo::Connection conn)
        : auth_(auth), conn_(std::move(conn))
    {
    }

    ~UserPassAuthenticator() {}

    /**
     * @brief Authenticate the username/password credentials in the call metadata.
     *
     * On success the username is stored in the auth context for per-RPC
     * authorization and the credentials are consumed so that they do not
     * reach the RPC handlers.
     *
     * @param[in] auth_metadata Call credentials metadata.
     * @param[in] context Authentication context to store the username in.
     * @param[out] consumed_auth_metadata Metadata consumed by the processor.
     * @param[out] response_metadata Unused response metadata.
     * @return OK when authenticated, UNAUTHENTICATED otherwise.
     */
    grpc::Status Process(const InputMetadata &auth_metadata, grpc::AuthContext *context,
        OutputMetadata *consumed_auth_metadata, OutputMetadata *response_metadata) override;

  private:
    const Auth &auth_;
    sysrepo::Connection conn_;
};
