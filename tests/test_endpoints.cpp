/**
 * @file test_endpoints.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Runtime listen endpoint management tests
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
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "test_main.h"

/**
 * @brief Wait until a fresh channel can no longer connect to an address.
 *
 * @param[in] addr Address to connect to.
 * @return True when a connection attempt failed within the timeout.
 */
static bool eventually_unreachable(const std::string &addr)
{
    auto channel = make_insecure_channel(addr);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!channel->WaitForConnected(
                std::chrono::system_clock::now() + std::chrono::milliseconds(100)))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TEST_CASE("Listen endpoint added at runtime", "[endpoints]")
{
    const std::string tcp_addr = "127.0.0.1:50053";

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='added']/ip/address", "127.0.0.1");
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='added']/ip/port", "50053");
    sr_sess->applyChanges();

    // the new endpoint is served by a new server without a restart
    REQUIRE(eventually_reachable(make_insecure_channel, tcp_addr));

    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='added']");
    sr_sess->applyChanges();

    REQUIRE(eventually_unreachable(tcp_addr));
}

TEST_CASE("Removed listen endpoint keeps streaming", "[endpoints]")
{
    const std::string tcp_addr = "127.0.0.1:50053";

    // the endpoint is removed while another one is configured, an empty
    // configuration would keep the previous endpoints instead
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='backup']/ip/address", "127.0.0.1");
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='backup']/ip/port", "50053");
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, tcp_addr));

    // an established gNMI subscription on the endpoint about to be removed
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = gnmi_client->Subscribe(&ctx);
    REQUIRE(rw->Write(request));

    // initial update and sync response
    REQUIRE(rw->Read(&response));
    REQUIRE(rw->Read(&response));
    REQUIRE(response.sync_response());

    // remove the endpoint the subscription is connected to
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']");
    sr_sess->applyChanges();

    // new connections are refused, gRPC unlinks the socket file
    REQUIRE(eventually_unreachable(insecure_addr));

    // the established subscription keeps streaming the changes
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='E']/counter", "42");
    sr_sess->applyChanges();
    CHECK(rw->Read(&response));
    CHECK(response.update().update_size() > 0);

    ctx.TryCancel();
    rw->Finish();

    // verify the endpoint is down
    REQUIRE(eventually_unreachable(insecure_addr));

    // the endpoint can be configured back
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']/unix/path",
        insecure_addr.substr(sizeof("unix:") - 1));
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, insecure_addr));
}
