/**
 * @file auth.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Authentication/authorization implementation
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
 * Copyright 2025 Graphiant Inc.
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

#include <filesystem>
#include <libyang-cpp/Enum.hpp>
#include <stdexcept>

#include <libyang/parser_schema.h>
#include <sysrepo.h>

#include <proto/gnmi.grpc.pb.h>

#include "utils/log.h"
#include "utils/sysrepo.h"

#include "auth.h"
#include "hash.h"
#include "utils/utils.h"

void Auth::loadUserDB()
{
    const std::lock_guard<std::mutex> lock(mutex_);

    try
    {
        // create a standalone context with only the userDB module loaded
        // find the YANG file via the sysrepo repository path
        std::filesystem::path yang_dir = std::filesystem::path(sr_get_repo_path()) / "yang";
        ctx_.emplace(yang_dir, libyang::ContextOptions::NoYangLibrary);
        ctx_->loadModule("sysrepo-gnxi-users");
        user_db_ = ctx_->parseData(std::filesystem::path(user_db_path), libyang::DataFormat::JSON);
        if (!user_db_.has_value())
        {
            throw;
        }
        SLOG_INFO("User database loaded from ", user_db_path);
    }
    catch (const std::exception &exc)
    {
        SLOG_FATAL("Failed to load user database from ", user_db_path, ": ", exc.what());
        throw std::runtime_error("failed to load the users database");
    }
}

std::shared_ptr<grpc::ServerCredentials> Auth::init()
{
    if (insecure)
    {
        SLOG_INFO("Insecure authentication");
        return grpc::InsecureServerCredentials();
    }
    else if (!private_key_path.empty() && !cert_path.empty() && !root_cert_path.empty() &&
             !user_db_path.empty())
    {
        SLOG_INFO("Mutual TLS authentication");
        loadUserDB();
        grpc::SslServerCredentialsOptions ssl_opts;
        ssl_opts.client_certificate_request =
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
        ssl_opts.pem_key_cert_pairs.push_back(
            {get_file_content(private_key_path), get_file_content(cert_path)});
        ssl_opts.pem_root_certs = get_file_content(root_cert_path);
        auto cred = grpc::SslServerCredentials(ssl_opts);
        cred->SetAuthMetadataProcessor(std::make_shared<UserPassAuthenticator>(*this));
        return cred;
    }
    else
    {
        SLOG_FATAL("Unsupported authentication method. Use insecure mode or provide "
                   "private key, certificate, CA certificate and a user database.");
        throw std::runtime_error("unsupported authentication method");
    }
}

std::string Auth::username(grpc::ServerContext *ctx) const
{
    assert(ctx);
    auto auth_ctx = ctx->auth_context();
    if (!auth_ctx)
    {
        return "";
    }
    auto vals = auth_ctx->FindPropertyValues("username");
    if (vals.empty())
    {
        return "";
    }
    return std::string(vals[0].data(), vals[0].length());
}

void Auth::authenticate(const std::string &username, const std::string &password) const
{
    const std::lock_guard<std::mutex> lock(mutex_);

    // insecure mode == no authentication
    if (insecure)
    {
        return;
    }

    auto users = user_db_->findXPath("/sysrepo-gnxi-users:users/user");
    for (const auto &user : users)
    {
        auto name = user.findPath("name");
        assert(name.has_value());
        if (name->asTerm().valueStr() != username)
        {
            continue;
        }

        auto stored_password = user.findPath("password");
        assert(stored_password.has_value());

        if (!check_hash(password, stored_password->asTerm().valueStr()))
        {
            SLOG_DEBUG("Invalid password for user '", username, "'");
            throw std::runtime_error("invalid password for '" + username + "'");
        }

        return; // authenticated
    }

    SLOG_DEBUG("User '", username, "' not found in user database");
    throw std::runtime_error("user not found");
}

void Auth::authorize(const std::string &username, const std::unordered_set<std::string> &modules,
                     Access permission) const
{
    const std::lock_guard<std::mutex> lock(mutex_);

    size_t modules_authorized = 0;

    try
    {
        auto users = user_db_->findXPath("/sysrepo-gnxi-users:users/user");
        for (const auto &user : users)
        {
            auto name = user.findPath("name");
            assert(name.has_value());
            if (name->asTerm().valueStr() != username)
            {
                continue;
            }

            auto acl_set = user.findXPath("acl");
            for (const auto &acl_node : acl_set)
            {
                auto mod_node = acl_node.findPath("module");
                assert(mod_node.has_value());
                std::string mod = mod_node->asTerm().valueStr();
                if (!modules.contains(mod))
                {
                    continue;
                }

                auto acc_node = acl_node.findPath("access");
                assert(acc_node.has_value());
                std::string acc = acc_node->asTerm().valueStr();

                // need read/write or read-only permission
                if ((permission == Auth::Access::ReadWrite && acc == "rw") ||
                    (permission == Auth::Access::ReadOnly && (acc == "rw" || acc == "ro")))
                {
                    ++modules_authorized;
                    continue;
                }
                // insufficient permissions
                else
                {
                    SLOG_WARN("Authorization failed (insufficient permissions): User " + username +
                              " has been denied " + acc + " access to " + mod + ".");
                    throw grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                       "User " + username + " has been denied " + acc +
                                           " access to " + mod + ".");
                }
            }

            // already found the user
            break;
        }
    }
    catch (grpc::Status &exc)
    {
        throw exc;
    }
    catch (std::exception &exc)
    {
        SLOG_ERROR("Authorization failed (user database problem): ", exc.what());
        throw grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                           "Authorization failed: " + std::string(exc.what()));
    }

    // authorized only if every used module has been covered by an ACL entry
    if (modules_authorized != modules.size())
    {
        SLOG_WARN("Authorization failed (module not in ACL): User ", username,
                  " lacks an ACL entry for at least one module.");
        throw grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                           "User " + username + " lacks an ACL entry for at least one module.");
    }
}

void Auth::authorize(grpc::ServerContext *ctx, const libyang::Context &ly_ctx,
                     const std::optional<gnmi::Path> &prefix, const std::vector<gnmi::Path> &paths,
                     Access permission) const
{
    // insecure mode == no authorization
    if (insecure)
    {
        return;
    }

    std::string xpaths_prefix;
    std::vector<std::string> xpaths;

    try
    {
        if (prefix.has_value())
        {
            xpaths_prefix = gnmi_to_xpath(prefix.value());
        }
        for (auto &path : paths)
        {
            xpaths.push_back(xpaths_prefix + gnmi_to_xpath(path));
        }
    }
    catch (const std::invalid_argument &exc)
    {
        SLOG_DEBUG("Prefix or path could not be parsed: ", exc.what());
        throw grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Prefix or path could not be parsed: ", exc.what());
    }

    // get username (otherwise we cannot authorize)
    std::string name = username(ctx);
    if (name.empty())
    {
        SLOG_ERROR("Authorization failed (no username)");
        throw grpc::Status(grpc::StatusCode::INTERNAL, "Authenticated username unavailable.");
    }

    for (const auto &xpath : xpaths)
    {
        // authorization of per XPath modules
        authorize(name, collect_xpath_mods(ly_ctx, xpath.c_str()), permission);
    }
}

grpc::Status UserPassAuthenticator::Process(const InputMetadata &auth_metadata,
                                            grpc::AuthContext *context,
                                            OutputMetadata *consumed_auth_metadata,
                                            OutputMetadata *response_metadata)
{
    (void)response_metadata;

    if (!auth_metadata.contains("username"))
    {
        SLOG_ERROR("No username field in metadata");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No username field");
    }
    if (!auth_metadata.contains("password"))
    {
        SLOG_ERROR("No password field in metadata");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No password field");
    }

    std::string username(auth_metadata.find("username")->second.data(),
                         auth_metadata.find("username")->second.length());
    std::string password(auth_metadata.find("password")->second.data(),
                         auth_metadata.find("password")->second.length());

    try
    {
        auth_.authenticate(username, password);
    }
    catch (const std::exception &exc)
    {
        SLOG_DEBUG("Authentication failed for user '", username, "': ", exc.what());
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid username/password");
    }

    // store username for per-RPC authorization
    context->AddProperty("username", username);

    // consume credentials so they don't reach the RPC handler metadata
    consumed_auth_metadata->insert(std::make_pair("username", username));
    consumed_auth_metadata->insert(std::make_pair("password", password));

    return grpc::Status::OK;
}
