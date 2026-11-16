/**
 * @file test_rotate.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Runtime TLS material rotation tests
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

#include <grpcpp/grpcpp.h>

#include "config.h"
#include "test_main.h"

TEST_CASE("TLS material rotation", "[rotate]")
{
    std::filesystem::path dir = TESTS_SCHEMA_DIR;

    // a client of the original hierarchy is served
    REQUIRE(do_capabilities(make_mtls_channel(mtls_addr), "alice", "alicepass").ok());

    const std::string original_key_path =
        "/ietf-keystore:keystore/asymmetric-keys/"
        "asymmetric-key[name='sysrepo-gnxi-server-key']/cleartext-private-key";
    auto original_key_data = sr_sess->getData(original_key_path);
    REQUIRE(original_key_data.has_value());
    const auto original_key = original_key_data->findPath(original_key_path)->asTerm().valueStr();

    // an established gNMI subscription on the original material
    grpc::ClientContext sub_ctx;
    prepare_context(sub_ctx, "alice", "alicepass");

    gnmi::SubscribeRequest request;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = gnmi::gNMI::NewStub(make_mtls_channel(mtls_addr))->Subscribe(&sub_ctx);
    REQUIRE(rw->Write(request));
    gnmi::SubscribeResponse response;
    REQUIRE(rw->Read(&response));
    REQUIRE(rw->Read(&response));
    REQUIRE(response.sync_response());

    // rotate the server identity and the client verification CA bundle
    apply_config(*sr_sess, dir / "rotated.json", "");

    // new connections of the new hierarchy are served
    REQUIRE(eventually_reachable(make_mtls_channel2, mtls_addr, "alice", "alicepass"));

    // new connections of the original hierarchy are rejected, both the
    // server identity and the client verification use the new material
    CHECK(!make_mtls_channel(mtls_addr)->WaitForConnected(
        std::chrono::system_clock::now() + std::chrono::milliseconds(100)));

    // the established subscription keeps streaming the changes
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='R']/counter", "42");
    sr_sess->applyChanges();
    CHECK(rw->Read(&response));
    CHECK(response.update().update_size() > 0);

    // an unusable rotation is rejected before it is committed, the
    // previously loaded material is kept
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/ietf-truststore:truststore/certificate-bags/"
        "certificate-bag[name=\"rotated'client-ca-bundle\"]/certificate[name='ca']/cert-data",
        "aW52YWxpZA==");
    CHECK_THROWS_WITH(sr_sess->applyChanges(),
        Catch::Matchers::Contains("gRPC cannot create a server with the new TLS material"));
    sr_sess->discardChanges();

    // the previous material is still served
    CHECK(do_capabilities(make_mtls_channel2(mtls_addr), "alice", "alicepass").ok());

    // changing only the key back leaves the rotated certificate paired with the original,
    // non-matching private key, so this candidate must not pass validation
    sr_sess->setItem("/ietf-keystore:keystore/asymmetric-keys/"
                     "asymmetric-key[name=\"rotated'server-key\"]/cleartext-private-key",
        original_key);
    CHECK_THROWS_WITH(sr_sess->applyChanges(),
        Catch::Matchers::Contains("gRPC cannot create a server with the new TLS material"));
    sr_sess->discardChanges();

    // client certificate pinning is valid in the enabled YANG schema, but is
    // not supported by the server and must be rejected explicitly
    sr_sess->setItem("/sysrepo-gnxi-server:server/tls/client-authentication/"
                     "ee-certs/central-truststore-reference",
        "rotated'client-ca-bundle");
    CHECK_THROWS_WITH(
        sr_sess->applyChanges(), Catch::Matchers::Contains("client-authentication/ee-certs"));
    sr_sess->discardChanges();

    // TLS keepalives are another unsupported TLS-server feature, they are
    // rejected by path rather than silently ignored
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/tls/keepalives/peer-allowed-to-send", std::nullopt);
    CHECK_THROWS_WITH(sr_sess->applyChanges(), Catch::Matchers::Contains("server/tls/keepalives"));
    sr_sess->discardChanges();

    // client authentication cannot simply be omitted by a secure server
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/tls/client-authentication");
    CHECK_THROWS_WITH(sr_sess->applyChanges(),
        Catch::Matchers::Contains("central truststore CA bundle is required"));
    sr_sess->discardChanges();

    // discarding the rejected edits leaves the working rotated hierarchy in use
    CHECK(do_capabilities(make_mtls_channel2(mtls_addr), "alice", "alicepass").ok());

    sub_ctx.TryCancel();
    rw->Finish();

    // clean up the operational test data
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='R']");
    sr_sess->applyChanges();
}
