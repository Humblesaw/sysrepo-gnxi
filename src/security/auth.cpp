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

#include <stdexcept>

#include <openssl/crypto.h>

#include <proto/gnmi.grpc.pb.h>

#include "utils/log.h"
#include "utils/sysrepo.h"
#include "utils/utils.h"

#include "auth.h"
#include "hash.h"

Auth::Auth(bool insecure, sysrepo::Connection conn, const TlsMaterial &tls) : insecure_(insecure)
{
    if (insecure_)
    {
        SLOG_INFO("Insecure authentication");
        credentials_ = grpc::InsecureServerCredentials();
        return;
    }

    if (tls.private_key_pem.empty() || tls.certificate_pem.empty() ||
        tls.ca_certificate_pem.empty())
    {
        SLOG_FATAL("Incomplete TLS configuration: private key, certificate, or CA bundle "
                   "is empty. Ensure the server configuration, keystore, and truststore are "
                   "properly configured in sysrepo.");
        throw std::runtime_error("incomplete TLS configuration");
    }

    SLOG_INFO("Mutual TLS authentication");
    grpc::SslServerCredentialsOptions ssl_opts;
    ssl_opts.client_certificate_request =
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    ssl_opts.pem_key_cert_pairs.push_back({tls.private_key_pem, tls.certificate_pem});
    ssl_opts.pem_root_certs = tls.ca_certificate_pem;
    credentials_ = grpc::SslServerCredentials(ssl_opts);
    // the metadata processor authenticates before an RPC sysrepo session exists
    credentials_->SetAuthMetadataProcessor(
        std::make_shared<UserPassAuthenticator>(*this, std::move(conn)));
}

std::shared_ptr<grpc::ServerCredentials> Auth::credentials() const
{
    return credentials_;
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

void Auth::authenticate(sysrepo::Session &sess, const std::string &username,
                        const std::string &password) const
{
    // insecure mode == no authentication
    if (insecure_)
    {
        return;
    }

    // read the user database live from sysrepo
    auto data = sess.getData("/sysrepo-gnxi-users:users");
    if (!data.has_value())
    {
        throw std::runtime_error("no user database found in sysrepo "
                                 "(/sysrepo-gnxi-users:users)");
    }

    auto users = data->findXPath("user");
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

void Auth::authorize(sysrepo::Session &sess, const std::string &username,
                     const std::unordered_set<std::string> &modules, Access permission) const
{
    size_t modules_authorized = 0;

    try
    {
        // read the user database live from sysrepo
        auto data = sess.getData("/sysrepo-gnxi-users:users");
        if (!data.has_value())
        {
            throw std::runtime_error("no user database found in sysrepo "
                                     "(/sysrepo-gnxi-users:users)");
        }

        auto users = data->findXPath("user");
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

void Auth::authorize(grpc::ServerContext *ctx, sysrepo::Session &sess,
                     const std::optional<gnmi::Path> &prefix, const std::vector<gnmi::Path> &paths,
                     Access permission) const
{
    // insecure mode == no authorization
    if (insecure_)
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
        auto mods = collect_xpath_mods(sess.getContext(), xpath.c_str());

        // reject any access to private modules
        for (const auto &mod : mods)
        {
            if (isPrivateModule(mod))
            {
                SLOG_WARN("Authorization failed (private module): User ", name,
                          " attempted access to private module '", mod, "'.");
                throw grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                   "Access to module '" + mod + "' is forbidden.");
            }
        }

        // authorize the remaining modules against the user's ACL
        authorize(sess, name, mods, permission);
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
    auto cleanse = [&username, &password]()
    {
        OPENSSL_cleanse(username.data(), username.size());
        OPENSSL_cleanse(password.data(), password.size());
    };

    try
    {
        // no RPC session exists at this point, authenticate with a fresh one
        auto sess = conn_.sessionStart(sysrepo::Datastore::Running);
        auth_.authenticate(sess, username, password);
    }
    catch (const std::exception &exc)
    {
        SLOG_DEBUG("Authentication failed for user '", username, "': ", exc.what());
        cleanse();
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid username/password");
    }

    // store username for per-RPC authorization
    context->AddProperty("username", username);

    // consume credentials so they don't reach the RPC handler metadata
    consumed_auth_metadata->insert(std::make_pair("username", username));
    consumed_auth_metadata->insert(std::make_pair("password", password));

    // wipe the local copies of the credentials
    cleanse();

    return grpc::Status::OK;
}
