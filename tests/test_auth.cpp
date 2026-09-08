/**
 * @file test_auth.cpp
 * @author Ondrej Kusnirik <kusnirik@cesnet.cz>
 * @brief Authentication/authorization tests
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

#include "catch2/catch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <grpcpp/client_context.h>
#include <grpcpp/support/sync_stream.h>
#include <memory>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>
#include <proto/gnmi_ext.pb.h>

#include "config.h"
#include "proto/gnmi.pb.h"
#include "test_main.h"

// helpers

/**
 * @brief Client certificate bundle.
 *
 */
struct PemBundle
{
    std::string ca_cert;
    std::string client_key;
    std::string client_cert;
};

/**
 * @brief Read a file into a string (PEM material for the gRPC channels).
 */
static std::string read_file(const std::filesystem::path &path)
{
    std::ifstream ifs(path);
    if (!ifs)
    {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(ifs), {}};
}

const PemBundle &pem_bundle()
{
    static PemBundle b = []()
    {
        std::filesystem::path dir = TESTS_SCHEMA_DIR;
        return PemBundle{
            .ca_cert = read_file(dir / "ca.crt"),
            .client_key = read_file(dir / "client.key"),
            .client_cert = read_file(dir / "client.crt"),
        };
    }();
    return b;
}

std::shared_ptr<grpc::Channel> make_insecure_channel(const std::string &addr)
{
    return grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
}

std::shared_ptr<grpc::Channel> make_tls_channel(const std::string &addr)
{
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = pem_bundle().ca_cert;
    return grpc::CreateChannel(addr, grpc::SslCredentials(opts));
}

std::shared_ptr<grpc::Channel> make_mtls_channel(const std::string &addr)
{
    const auto &b = pem_bundle();
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = b.ca_cert;
    opts.pem_private_key = b.client_key;
    opts.pem_cert_chain = b.client_cert;
    return grpc::CreateChannel(addr, grpc::SslCredentials(opts));
}

void prepare_context(grpc::ClientContext &ctx, const std::string &username = "",
                     const std::string &password = "")
{
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    if (!username.empty())
    {
        ctx.AddMetadata("username", username);
    }
    if (!password.empty())
    {
        ctx.AddMetadata("password", password);
    }
}

grpc::Status do_capabilities(const std::shared_ptr<grpc::Channel> &channel,
                             const std::string &username = "", const std::string &password = "")
{
    auto stub = gnmi::gNMI::NewStub(channel);
    grpc::ClientContext ctx;
    prepare_context(ctx, username, password);
    gnmi::CapabilityRequest request;
    gnmi::CapabilityResponse response;
    return stub->Capabilities(&ctx, request, &response);
}

grpc::Status do_get(const std::shared_ptr<grpc::Channel> &channel, std::string xpath,
                    const std::string &username = "", const std::string &password = "")
{
    auto stub = gnmi::gNMI::NewStub(channel);
    grpc::ClientContext ctx;
    prepare_context(ctx, username, password);
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(xpath, request.add_path());
    gnmi::GetResponse response;
    return stub->Get(&ctx, request, &response);
}

grpc::Status do_set(const std::shared_ptr<grpc::Channel> &channel, std::string xpath,
                    std::string value, const std::string &username = "",
                    const std::string &password = "")
{
    auto stub = gnmi::gNMI::NewStub(channel);
    grpc::ClientContext ctx;
    prepare_context(ctx, username, password);
    gnmi::SetRequest request;
    auto upd = request.add_update();
    xpath_to_path(xpath, upd->mutable_path());
    upd->mutable_val()->set_json_ietf_val(value);
    gnmi::SetResponse response;
    return stub->Set(&ctx, request, &response);
}

std::unique_ptr<grpc::ClientReaderWriter<gnmi::SubscribeRequest, gnmi::SubscribeResponse>>
do_subscribe(const std::shared_ptr<grpc::Channel> &channel, grpc::ClientContext &ctx,
             std::string xpath, gnmi::SubscribeRequest &request, const std::string &username = "",
             const std::string &password = "")
{
    auto stub = gnmi::gNMI::NewStub(channel);
    prepare_context(ctx, username, password);
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(xpath, sub->mutable_path());
    return stub->Subscribe(&ctx);
}

// positive tests

TEST_CASE("mTLS: valid cert + valid credentials (sha512-hashed password)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_capabilities(ch, "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: valid cert + valid credentials (md5-hashed password)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_capabilities(ch, "chuck", "chuck$pass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: authorized Get RPC (rw)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_get(ch, "/gnmi-server-test:test/things", "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: authorized Get RPC (ro)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_get(ch, "/gnmi-server-test:test/things", "bob", "bobpass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: denied Get RPC", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_get(ch, "/gnmi-server-test:test/things", "chuck", "chuck$pass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: authorized Set RPC (rw)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_set(ch, "/gnmi-server-test:test/things[name='auth-set-pos']/description",
                         "\"set by alice\"", "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: denied Set RPC (ro)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_set(ch, "/gnmi-server-test:test/things[name='auth-set-pos']/description",
                         "\"set by bob\"", "bob", "bobpass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: denied Set RPC", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_set(ch, "/gnmi-server-test:test/things[name='auth-set-pos']/description",
                         "\"set by chuck\"", "chuck", "chuck$pass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: authorized Subscribe (rw)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    gnmi::SubscribeRequest request;
    grpc::ClientContext ctx;
    auto rw = do_subscribe(ch, ctx, "/gnmi-server-test:test-state", request, "alice", "alicepass");
    CHECK(rw->Write(request));

    gnmi::SubscribeResponse response;
    bool got_update = false;
    while (rw->Read(&response))
    {
        if (response.has_update())
        {
            got_update = true;
        }
        if (response.sync_response())
        {
            break;
        }
    }
    CHECK(got_update);

    rw->WritesDone();
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("mTLS: authorized Subscribe (ro)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    gnmi::SubscribeRequest request;
    grpc::ClientContext ctx;
    auto rw = do_subscribe(ch, ctx, "/gnmi-server-test:test-state", request, "bob", "bobpass");
    CHECK(rw->Write(request));

    gnmi::SubscribeResponse response;
    bool got_update = false;
    while (rw->Read(&response))
    {
        if (response.has_update())
        {
            got_update = true;
        }
        if (response.sync_response())
        {
            break;
        }
    }
    CHECK(got_update);

    rw->WritesDone();
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("mTLS: denied Subscribe", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    gnmi::SubscribeRequest request;
    grpc::ClientContext ctx;
    auto rw = do_subscribe(ch, ctx, "/gnmi-server-test:test-state", request, "chuck", "chuck$pass");
    (void)rw->Write(request);

    gnmi::SubscribeResponse response;
    CHECK(!rw->Read(&response));

    rw->WritesDone();
    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

// negative tests

TEST_CASE("mTLS: valid cert + wrong password", "[auth-neg]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_capabilities(ch, "alice", "wrongpass");
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("mTLS: valid cert + unknown user", "[auth-neg]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_capabilities(ch, "nosuchuser", "wrongpass");
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("mTLS: valid cert + no credentials", "[auth-neg]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_capabilities(ch);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("mTLS: no client cert", "[auth-neg]")
{
    auto ch = make_tls_channel(mtls_addr);
    auto status = do_capabilities(ch, "alice", "alicepass");
    CHECK(!status.ok());
}

TEST_CASE("Insecure client to mTLS port", "[auth-neg]")
{
    auto ch = make_insecure_channel(mtls_addr);
    auto status = do_capabilities(ch);
    CHECK(!status.ok());
}

TEST_CASE("mTLS: commit confirm is bound to the initiating user", "[auth-commit]")
{
    auto alice = gnmi::gNMI::NewStub(make_mtls_channel(mtls_addr));
    auto bob = gnmi::gNMI::NewStub(make_mtls_channel(mtls_addr));
    const std::string commit_id = "bind-test";

    // alice
    {
        grpc::ClientContext ctx;
        prepare_context(ctx, "alice", "alicepass");
        gnmi::SetRequest request;
        auto commit = request.add_extension()->mutable_commit();
        commit->set_id(commit_id);
        commit->mutable_commit();
        auto upd = request.add_update();
        xpath_to_path("/gnmi-server-test:test/things[name='bind-test']/description",
                      upd->mutable_path());
        upd->mutable_val()->set_json_ietf_val("\"confirm binding\"");
        gnmi::SetResponse response;
        auto status = alice->Set(&ctx, request, &response);
        CHECK(status.ok());
    }

    // bob (confirm must be rejected)
    {
        grpc::ClientContext ctx;
        prepare_context(ctx, "bob", "bobpass");
        gnmi::SetRequest request;
        auto commit = request.add_extension()->mutable_commit();
        commit->set_id(commit_id);
        commit->mutable_confirm();
        gnmi::SetResponse response;
        auto status = bob->Set(&ctx, request, &response);
        CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
    }
}

TEST_CASE("mTLS: unknown user and wrong password are indistinguishable", "[auth-neg]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto unknown = do_capabilities(ch, "nosuchuser", "wrongpass");
    auto wrong_password = do_capabilities(ch, "alice", "wrongpass");
    CHECK(unknown.error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(wrong_password.error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(unknown.error_message() == wrong_password.error_message());
}
