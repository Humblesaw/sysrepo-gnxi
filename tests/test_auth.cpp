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
#include <grpc/impl/codegen/grpc_types.h>
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
 *
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

/**
 * @brief Get the test client certificate bundle, loaded once from the test schema directory.
 *
 * @return Test client certificate bundle.
 */
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

/**
 * @brief Create an insecure gRPC channel to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
std::shared_ptr<grpc::Channel> make_insecure_channel(const std::string &addr)
{
    return grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
}

/**
 * @brief Create a TLS gRPC channel without a client certificate to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
std::shared_ptr<grpc::Channel> make_tls_channel(const std::string &addr)
{
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = pem_bundle().ca_cert;
    return grpc::CreateChannel(addr, grpc::SslCredentials(opts));
}

/**
 * @brief Create an mTLS gRPC channel with the test client certificate to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
std::shared_ptr<grpc::Channel> make_mtls_channel(const std::string &addr)
{
    const auto &b = pem_bundle();
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = b.ca_cert;
    opts.pem_private_key = b.client_key;
    opts.pem_cert_chain = b.client_cert;
    // force a dedicated connection for each channel, gRPC otherwise pools
    // transports of identical credentials and a call's auth-context metadata
    // (the authenticated username) is bound to the first RPC on the shared
    // connection which breaks per-user NACM expectations in the tests
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    return grpc::CreateCustomChannel(addr, grpc::SslCredentials(opts), args);
}

/**
 * @brief Prepare a client context with a deadline and optional credentials metadata.
 *
 * @param[in] ctx Client context to prepare.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 */
void prepare_context(
    grpc::ClientContext &ctx, const std::string &username = "", const std::string &password = "")
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

/**
 * @brief Call the Capabilities RPC on a channel with optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return RPC status.
 */
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

/**
 * @brief Call the Get RPC for a config path on a channel with optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] xpath XPath to get.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return RPC status.
 */
grpc::Status do_get(const std::shared_ptr<grpc::Channel> &channel, std::string xpath,
    const std::string &username = "", const std::string &password = "",
    gnmi::GetResponse *out = nullptr)
{
    auto stub = gnmi::gNMI::NewStub(channel);
    grpc::ClientContext ctx;
    prepare_context(ctx, username, password);
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(xpath, request.add_path());
    gnmi::GetResponse response;
    auto status = stub->Get(&ctx, request, &response);
    if (out)
    {
        *out = response;
    }
    return status;
}

/**
 * @brief Call the Rpc RPC on a channel with optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] xpath XPath of the RPC to invoke.
 * @param[in] input_json JSON-IETF input of the RPC.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return RPC status.
 */
grpc::Status do_rpc(const std::shared_ptr<grpc::Channel> &channel, std::string xpath,
    std::string input_json, const std::string &username = "", const std::string &password = "")
{
    auto stub = yang_rpc::YANG_RPC::NewStub(channel);
    grpc::ClientContext ctx;
    prepare_context(ctx, username, password);
    yang_rpc::RpcRequest request;
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(xpath, request.mutable_path());
    request.mutable_input()->set_json_ietf_val(input_json);
    yang_rpc::RpcResponse response;
    return stub->Rpc(&ctx, request, &response);
}

/**
 * @brief Call the Set RPC with a single update on a channel with optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] xpath XPath to set.
 * @param[in] value JSON-IETF value to set.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return RPC status.
 */
grpc::Status do_set(const std::shared_ptr<grpc::Channel> &channel, std::string xpath,
    std::string value, const std::string &username = "", const std::string &password = "")
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

/**
 * @brief Prepare a ONCE Subscribe request and open the Subscribe stream on a channel with
 * optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] ctx Client context to prepare and use for the stream.
 * @param[in] xpath XPath to subscribe to.
 * @param[in] request Subscribe request to fill with the subscription.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return New Subscribe stream.
 */
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
    auto status = do_get(ch, "/sysrepo-gnxi-test:test/things", "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: authorized Get RPC (ro)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_get(ch, "/sysrepo-gnxi-test:test/things", "bob", "bobpass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: unauthorized Get is silently filtered (NACM)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    // chuck matches no rule-list, unauthorized reads are not an error, the
    // data are filtered out by NACM
    gnmi::GetResponse response;
    auto status = do_get(ch, "/sysrepo-gnxi-test:test/things", "chuck", "chuck$pass", &response);
    CHECK(status.ok());
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification(0).update_size() == 0);
}

TEST_CASE("mTLS: private modules are readable only with explicit NACM rules", "[auth]")
{
    // one channel per identity
    auto alice_ch = make_mtls_channel(mtls_addr);
    auto bob_ch = make_mtls_channel(mtls_addr);

    // alice is in the admin group with a permit-all rule
    gnmi::GetResponse response;
    auto status = do_get(alice_ch, "/ietf-keystore:keystore", "alice", "alicepass", &response);
    CHECK(status.ok());
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification(0).update_size() > 0);

    // bob may read sysrepo-gnxi-test only, the keystore is silently filtered out
    gnmi::GetResponse denied_response;
    status = do_get(bob_ch, "/ietf-keystore:keystore", "bob", "bobpass", &denied_response);
    CHECK(status.ok());
    REQUIRE(denied_response.notification_size() == 1);
    CHECK(denied_response.notification(0).update_size() == 0);
}

TEST_CASE("mTLS: authorized Rpc (NACM exec)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_rpc(
        ch, "/sysrepo-gnxi-test:clear-stats", "{\"interface\": \"eth0\"}", "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: denied Rpc (NACM exec)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    // bob is read-only on the module, exec-default denies the rest
    auto status =
        do_rpc(ch, "/sysrepo-gnxi-test:clear-stats", "{\"interface\": \"eth0\"}", "bob", "bobpass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);

    // chuck is not in any NACM group at all
    status = do_rpc(
        ch, "/sysrepo-gnxi-test:clear-stats", "{\"interface\": \"eth0\"}", "chuck", "chuck$pass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: authorized Set RPC (rw)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_set(ch, "/sysrepo-gnxi-test:test/things[name='auth-set-pos']/description",
        "\"set by alice\"", "alice", "alicepass");
    CHECK(status.ok());
}

TEST_CASE("mTLS: denied Set RPC (read-only user)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    // bob has a NACM read permit for the module, writes are denied
    auto status = do_set(ch, "/sysrepo-gnxi-test:test/things[name='auth-set-pos']/description",
        "\"set by bob\"", "bob", "bobpass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: denied Set RPC (user without rules)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    auto status = do_set(ch, "/sysrepo-gnxi-test:test/things[name='auth-set-pos']/description",
        "\"set by chuck\"", "chuck", "chuck$pass");
    CHECK(status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("mTLS: authorized Subscribe (rw)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    gnmi::SubscribeRequest request;
    grpc::ClientContext ctx;
    auto rw = do_subscribe(ch, ctx, "/sysrepo-gnxi-test:test-state", request, "alice", "alicepass");
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
    auto rw = do_subscribe(ch, ctx, "/sysrepo-gnxi-test:test-state", request, "bob", "bobpass");
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

TEST_CASE("mTLS: unauthorized Subscribe is silently filtered (NACM)", "[auth]")
{
    auto ch = make_mtls_channel(mtls_addr);
    gnmi::SubscribeRequest request;
    grpc::ClientContext ctx;
    // chuck matches no rule-list, the stream opens and closes normally, but
    // unreadable data never reach it
    auto rw =
        do_subscribe(ch, ctx, "/sysrepo-gnxi-test:test-state", request, "chuck", "chuck$pass");
    REQUIRE(rw->Write(request));

    gnmi::SubscribeResponse response;
    bool got_update = false;
    while (rw->Read(&response))
    {
        got_update = got_update || (response.has_update() && response.update().update_size() > 0);
        if (response.sync_response())
        {
            break;
        }
    }
    CHECK(!got_update);

    rw->WritesDone();
    auto status = rw->Finish();
    CHECK(status.ok());
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
        xpath_to_path(
            "/sysrepo-gnxi-test:test/things[name='bind-test']/description", upd->mutable_path());
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
