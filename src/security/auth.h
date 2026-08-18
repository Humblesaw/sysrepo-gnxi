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

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_context.h>

#include <proto/gnmi.grpc.pb.h>

#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/DataNode.hpp>

class Auth
{
  public:
    enum class Access
    {
        ReadOnly,
        ReadWrite,
    };

    std::string private_key_path, cert_path, root_cert_path, user_db_path;
    bool insecure = false;

    /**
     * @brief Initialize authentication method (TLS or insecure) and load users database.
     *
     * @return gRPC server credentials.
     */
    std::shared_ptr<grpc::ServerCredentials> init();

    /**
     * @brief Get the authenticated username from the gRPC auth context.
     *
     * @param[in] ctx Server context to read the username from.
     * @return Authenticated username, or an empty string when unavailable/insecure.
     */
    std::string username(grpc::ServerContext *ctx) const;

    /**
     * @brief Authenticate username and password against a database of known users. Throw on error.
     *
     * @param[in] username Username to authenticate.
     * @param[in] password Password to authenticate.
     */
    void authenticate(const std::string &username, const std::string &password) const;

    /**
     * @brief Authorize an operation against a database of known users. Throw on error.
     *
     * @param[in] ctx Server context to read the username from.
     * @param[in] ly_ctx Libyang context to collect all module names needed for the authorization
     * request.
     * @param[in] prefix Prefix of the paths to check.
     * @param[in] paths Paths to check.
     * @param[in] permission Permission level needed to authorize the process (read/write).
     */
    void authorize(grpc::ServerContext *ctx, const libyang::Context &ly_ctx,
                   const std::optional<gnmi::Path> &prefix, const std::vector<gnmi::Path> &paths,
                   Access permission) const;

  private:
    mutable std::mutex mutex_;
    std::optional<libyang::Context> ctx_;
    std::optional<libyang::DataNode> user_db_;

    /**
     * @brief Load the users database for authentication/authorization purposes.
     */
    void loadUserDB();

    /**
     * @brief Authorize an operation (read/write) against a database of known users. Throw on error.
     *
     * @param[in] username Username to authorize.
     * @param[in] modules Names of the modules used in the operation.
     * @param[in] permission Permission level needed to authorize the process (read/write).
     */
    void authorize(const std::string &username, const std::unordered_set<std::string> &modules,
                   Access permission) const;
};

class UserPassAuthenticator final : public grpc::AuthMetadataProcessor
{
  public:
    explicit UserPassAuthenticator(const Auth &auth) : auth_(auth) {}
    ~UserPassAuthenticator() {}

    grpc::Status Process(const InputMetadata &auth_metadata, grpc::AuthContext *context,
                         OutputMetadata *consumed_auth_metadata,
                         OutputMetadata *response_metadata) override;

  private:
    const Auth &auth_;
};
