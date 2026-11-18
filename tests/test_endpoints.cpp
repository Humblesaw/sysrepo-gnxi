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
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <sysrepo-cpp/Subscription.hpp>

#include "config.h"
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

/**
 * @brief Fixture: restores the insecure test configuration before and after
 * every test case.
 *
 */
class EndpointFixture
{
  public:
    EndpointFixture() { reset_config_(); }

    ~EndpointFixture() { reset_config_(); }

  private:
    /**
     * @brief Reset endpoint configuration to the insecure test fixture.
     *
     */
    static void reset_config_()
    {
        // rejected test edits are kept in the session until discarded
        sr_sess->discardChanges();
        apply_config(*sr_sess, std::filesystem::path(TESTS_SCHEMA_DIR) / "insecure.json",
            insecure_addr.substr(sizeof("unix:") - 1));
    }
};

/**
 * @brief Address of an auxiliary endpoint's per-test unix socket.
 *
 * @param[in] name Endpoint-specific socket name.
 * @return gRPC listen address.
 */
static std::string endpoint_addr(const std::string &name)
{
    return "unix:" +
           (std::filesystem::path(TESTS_WORKING_DIR) / ("test_endpoints-" + name + ".sock"))
               .string();
}

TEST_CASE_METHOD(EndpointFixture, "Listen endpoint added at runtime", "[endpoints]")
{
    const auto added_addr = endpoint_addr("added");

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='added']/unix/path",
        added_addr.substr(sizeof("unix:") - 1));
    sr_sess->applyChanges();

    // the new endpoint is served by a new server without a restart
    REQUIRE(eventually_reachable(make_insecure_channel, added_addr));

    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='added']");
    sr_sess->applyChanges();

    REQUIRE(eventually_unreachable(added_addr));
}

TEST_CASE_METHOD(EndpointFixture, "Removed listen endpoint keeps streaming", "[endpoints]")
{
    const auto backup_addr = endpoint_addr("backup");

    // the original endpoint is removed while the backup endpoint remains
    // available for new connections
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='backup']/unix/path",
        backup_addr.substr(sizeof("unix:") - 1));
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, backup_addr));

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

TEST_CASE_METHOD(EndpointFixture, "Empty listen endpoint set is rejected", "[endpoints]")
{
    const auto backup_addr = endpoint_addr("backup");

    // both endpoints exist before the attempted wipe, irrespective of which
    // cases ran before this one
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='backup']/unix/path",
        backup_addr.substr(sizeof("unix:") - 1));
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, backup_addr));

    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']");
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='backup']");

    try
    {
        sr_sess->applyChanges();
        FAIL("wiping all the listen endpoints was not rejected");
    }
    catch (const std::exception &exc)
    {
        CHECK_THAT(exc.what(), Catch::Matchers::Contains("Too few \"endpoint\" instances"));
    }

    // nothing was changed, both endpoints are still served
    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
    CHECK(eventually_reachable(make_insecure_channel, backup_addr));
}

TEST_CASE_METHOD(EndpointFixture, "Unbindable listen endpoint is rejected", "[endpoints]")
{
    // syntactically valid configuration whose address cannot be bound locally
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='unbindable']/ip/address", "192.0.2.1");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='unbindable']/ip/port", "50053");

    try
    {
        sr_sess->applyChanges();
        FAIL("the unbindable listen endpoint was not rejected");
    }
    catch (const std::exception &exc)
    {
        CHECK_THAT(
            exc.what(), Catch::Matchers::Contains("failed to start a listen endpoint server"));
    }

    // the candidate was rolled back and the original endpoint is still served
    CHECK(eventually_unreachable("192.0.2.1:50053"));
    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
}

TEST_CASE_METHOD(
    EndpointFixture, "Listen change denied by another subscriber is reverted", "[endpoints]")
{
    const auto aborted_addr = endpoint_addr("aborted");

    sr_sess->switchDatastore(sysrepo::Datastore::Running);

    // the subscription is registered after the server's listen
    // subscription, so the denial necessarily comes after the new endpoint
    // was applied and its revert is exercised
    auto denial = sr_sess->onModuleChange(
        "sysrepo-gnxi-test",
        [](sysrepo::Session session, uint32_t, std::string_view, std::optional<std::string_view>,
            sysrepo::Event event, uint32_t)
        {
            if (event == sysrepo::Event::Change)
            {
                session.setErrorMessage("deliberate denial of the listen change");
                return sysrepo::ErrorCode::CallbackFailed;
            }
            return sysrepo::ErrorCode::Ok;
        },
        "/sysrepo-gnxi-test:test4/params");

    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='aborted']/unix/path",
        aborted_addr.substr(sizeof("unix:") - 1));
    sr_sess->setItem("/sysrepo-gnxi-test:test4/params", "to-be-denied");

    try
    {
        sr_sess->applyChanges();
        FAIL("the change denied by another subscriber was not rejected");
    }
    catch (const std::exception &exc)
    {
        CHECK_THAT(exc.what(), Catch::Matchers::Contains("deliberate denial of the listen change"));
    }

    // the endpoint created before the denial was reverted again
    CHECK(eventually_unreachable(aborted_addr));
    // the rejecting subscription is destroyed at the end of this case
}

TEST_CASE_METHOD(EndpointFixture, "Transport choice and IP address are mandatory", "[endpoints]")
{
    sr_sess->switchDatastore(sysrepo::Datastore::Running);

    // no transport choice selected
    auto invalid = sr_sess->getContext().parseData(
        std::string("{\"sysrepo-gnxi-server:server\":{\"listen\":{\"endpoint\":[{\"name\":\"no-"
                    "transport\"}]}}}"),
        libyang::DataFormat::JSON, libyang::ParseOptions::ParseOnly | libyang::ParseOptions::Strict,
        std::nullopt);
    sr_sess->editBatch(invalid.value(), sysrepo::DefaultOperation::Merge);
    CHECK_THROWS_WITH(sr_sess->applyChanges(), Catch::Matchers::Contains("Mandatory"));

    // selected IP transport but missing the required address
    sr_sess->discardChanges();
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='no-address']/ip/port", "50053");
    CHECK_THROWS_WITH(sr_sess->applyChanges(), Catch::Matchers::Contains("Mandatory"));

    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
}

TEST_CASE_METHOD(EndpointFixture, "Listen endpoint defaults its port", "[endpoints]")
{
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='default-port']/ip/address", "127.0.0.2");
    sr_sess->applyChanges();

    // the schema supplies 50051 as an implicit-default leaf
    CHECK(eventually_reachable(make_insecure_channel, "127.0.0.2:50051"));
}
