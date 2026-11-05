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
        "certificate-bag[name='sysrepo-gnxi-client-ca-bundle']/certificate[name='ca']/cert-data",
        "aW52YWxpZA==");
    try
    {
        sr_sess->applyChanges();
        FAIL("the rotation that gRPC cannot serve was not rejected");
    }
    catch (const std::exception &exc)
    {
        CHECK_THAT(exc.what(),
            Catch::Matchers::Contains("gRPC cannot create a server with the new TLS material"));
    }

    // the previous material is still served
    CHECK(do_capabilities(make_mtls_channel2(mtls_addr), "alice", "alicepass").ok());

    sub_ctx.TryCancel();
    rw->Finish();

    // clean up the operational test data
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='R']");
    sr_sess->applyChanges();
}
