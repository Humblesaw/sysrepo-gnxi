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

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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
 * @brief Occupy a test TCP port as soon as its previous listener releases it.
 *
 */
class PortBlocker
{
  public:
    /**
     * @brief Bind and listen on 127.0.0.2 without allowing port reuse.
     *
     * @param[in] port TCP port to occupy.
     */
    explicit PortBlocker(uint16_t port)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.2", &address.sin_addr);

        while (true)
        {
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd_ < 0)
            {
                throw std::runtime_error("failed to create the port blocker");
            }

            int enabled = 1;
            (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
            if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            {
                break;
            }

            ::close(fd_);
            fd_ = -1;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error("failed to occupy the original listen port");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (::listen(fd_, 1) != 0)
        {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("failed to listen on the original port");
        }
    }

    PortBlocker(const PortBlocker &) = delete;
    PortBlocker &operator=(const PortBlocker &) = delete;

    /**
     * @brief Release the occupied port.
     */
    ~PortBlocker()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

  private:
    int fd_ = -1;
};

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
        sr_sess->switchDatastore(sysrepo::Datastore::Running);
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

TEST_CASE_METHOD(
    EndpointFixture, "Endpoint drain cancels streaming RPCs with a deadline", "[endpoints]")
{
    const auto backup_addr = endpoint_addr("backup");

    // the original endpoint will be removed while the backup endpoint remains
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

    // establish one subscription on the endpoint about to be removed
    auto rw = gnmi_client->Subscribe(&ctx);
    REQUIRE(rw->Write(request));

    // initial update and sync response
    REQUIRE(rw->Read(&response));
    REQUIRE(rw->Read(&response));
    REQUIRE(response.sync_response());

    // remove the endpoint the subscription is connected to
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']");
    sr_sess->applyChanges();

    // new connections are refused and the removed listener's stream terminates
    REQUIRE(eventually_unreachable(insecure_addr));
    CHECK_FALSE(rw->Read(&response));
    CHECK_FALSE(rw->Finish().ok());

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
    CHECK_THROWS_WITH(
        sr_sess->applyChanges(), Catch::Matchers::Contains("Too few \"endpoint\" instances"));

    // nothing was changed, both endpoints are still served
    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
    CHECK(eventually_reachable(make_insecure_channel, backup_addr));
}

TEST_CASE_METHOD(EndpointFixture, "Unbindable listen endpoint is rejected", "[endpoints]")
{
    // replace the existing endpoint with a syntactically valid configuration
    // whose address cannot be bound locally, failure must not drain the original
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='unbindable']/ip/address", "192.0.2.1");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='unbindable']/ip/port", "50053");
    CHECK_THROWS_WITH(sr_sess->applyChanges(),
        Catch::Matchers::Contains("failed to start a listen endpoint server"));

    // the candidate was rolled back and the original endpoint is still served
    CHECK(eventually_unreachable("192.0.2.1:50053"));
    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
}

// TODO cannot restore reliably since the server ends all running RPCs in 100ms (uncomment when
// there is configuration available which could modify the deadline)
// TEST_CASE_METHOD(
//     EndpointFixture, "Removed endpoint can be restored while still draining", "[endpoints]")
// {
//     const auto backup_addr = endpoint_addr("rollback");

//     // keep an RPC in flight so the original server does not finish draining
//     grpc::ClientContext ctx;
//     gnmi::SubscribeRequest request;
//     gnmi::SubscribeResponse response;
//     auto list = request.mutable_subscribe();
//     auto sub = list->add_subscription();
//     list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
//     list->set_encoding(gnmi::Encoding::JSON_IETF);
//     xpath_to_path("/sysrepo-gnxi-test:test-state", sub->mutable_path());
//     sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

//     auto rw = gnmi_client->Subscribe(&ctx);
//     REQUIRE(rw->Write(request));
//     REQUIRE(rw->Read(&response));
//     REQUIRE(rw->Read(&response));
//     REQUIRE(response.sync_response());

//     // remove insecure while backup remains, its original server starts
//     // draining but cannot finish while this subscription is active
//     sr_sess->switchDatastore(sysrepo::Datastore::Running);
//     sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']");
//     sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='backup']/unix/path",
//         backup_addr.substr(sizeof("unix:") - 1));
//     sr_sess->applyChanges();

//     REQUIRE(eventually_reachable(make_insecure_channel, backup_addr));
//     REQUIRE(eventually_unreachable(insecure_addr));

//     // restore insecure while its original server is still draining, its unix
//     // socket must be rebound without the old server removing the new path
//     sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='insecure']/unix/path",
//         insecure_addr.substr(sizeof("unix:") - 1));
//     sr_sess->applyChanges();

//     REQUIRE(eventually_reachable(make_insecure_channel, backup_addr));
//     REQUIRE(eventually_reachable(make_insecure_channel, insecure_addr));

//     // draining and restoring the original server did not affect its established RPC
//     sr_sess->switchDatastore(sysrepo::Datastore::Operational);
//     sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='D']/counter", "42");
//     sr_sess->applyChanges();
//     CHECK(rw->Read(&response));
//     CHECK(response.update().update_size() > 0);

//     ctx.TryCancel();
//     rw->Finish();
//     REQUIRE(eventually_reachable(make_insecure_channel, insecure_addr));
// }

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
    CHECK_THROWS_WITH(sr_sess->applyChanges(),
        Catch::Matchers::Contains("deliberate denial of the listen change"));

    // the endpoint created before the denial was reverted again
    CHECK(eventually_unreachable(aborted_addr));
    // the rejecting subscription is destroyed at the end of this case
}

TEST_CASE_METHOD(
    EndpointFixture, "Failed abort rollback drains uncommitted listeners", "[endpoints]")
{
    const auto original_addr = "127.0.0.2:50058";
    const auto candidate_addr = "127.0.0.2:50059";
    std::mutex mutex;
    std::condition_variable condition;
    bool change_applied = false;
    bool reject_change = false;

    // keep the unix endpoint as an independent way to reach the server
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='original']/ip/address", "127.0.0.2");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='original']/ip/port", "50058");
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, original_addr));

    // block the later subscriber after the candidate endpoint has replaced the
    // original listener
    auto denial = sr_sess->onModuleChange(
        "sysrepo-gnxi-test",
        [&](sysrepo::Session session, uint32_t, std::string_view, std::optional<std::string_view>,
            sysrepo::Event event, uint32_t)
        {
            if (event != sysrepo::Event::Change)
            {
                return sysrepo::ErrorCode::Ok;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                change_applied = true;
            }
            condition.notify_one();
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&] { return reject_change; });
            }
            session.setErrorMessage("deliberate denial of the listen change");
            return sysrepo::ErrorCode::CallbackFailed;
        },
        "/sysrepo-gnxi-test:test4/params");

    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='original']");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='candidate']/ip/address", "127.0.0.2");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='candidate']/ip/port", "50059");
    sr_sess->setItem("/sysrepo-gnxi-test:test4/params", "to-be-denied");
    auto applying = std::async(std::launch::async, [&] { sr_sess->applyChanges(); });

    {
        std::unique_lock<std::mutex> lock(mutex);
        auto applied =
            condition.wait_for(lock, std::chrono::seconds(2), [&] { return change_applied; });
        if (!applied)
        {
            // in case of timeout, unlock everything to avoid stalling the test
            reject_change = true;
            lock.unlock();
            condition.notify_one();
            applying.wait();
        }
        REQUIRE(applied);
    }

    {
        // restoring the committed original listener must fail while its port is
        // occupied by a socket that does not share it with gRPC
        PortBlocker blocker(50058);

        {
            std::lock_guard<std::mutex> lock(mutex);
            reject_change = true;
        }
        condition.notify_one();
        CHECK_THROWS_WITH(
            applying.get(), Catch::Matchers::Contains("deliberate denial of the listen change"));

        // even though the committed preimage could not be fully restored, the
        // uncommitted candidate cannot keep accepting new connections
        CHECK(eventually_unreachable(candidate_addr));
        CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
    }

    // a later successful change restores the missing committed listener
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->discardChanges();
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='original']/ip/port", "50060");
    sr_sess->applyChanges();
    CHECK(eventually_reachable(make_insecure_channel, "127.0.0.2:50060"));
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

TEST_CASE_METHOD(EndpointFixture, "Duplicate URIs get distinct servers", "[endpoints]")
{
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='source']/ip/address", "127.0.0.2");
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='source']/ip/port", "50056");
    sr_sess->applyChanges();

    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='renamed']/ip/address", "127.0.0.2");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='renamed']/ip/port", "50056");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='duplicate']/ip/address", "127.0.0.2");
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='duplicate']/ip/port", "50056");
    sr_sess->deleteItem("/sysrepo-gnxi-server:server/listen/endpoint[name='source']");
    sr_sess->applyChanges();
}

TEST_CASE_METHOD(EndpointFixture, "Duplicate unix URIs are rejected", "[endpoints]")
{
    const auto duplicate_addr = endpoint_addr("duplicate");

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='original']/unix/path",
        duplicate_addr.substr(sizeof("unix:") - 1));
    sr_sess->applyChanges();
    REQUIRE(eventually_reachable(make_insecure_channel, duplicate_addr));

    // no candidate server may bind or drain the original server's socket path
    sr_sess->setItem("/sysrepo-gnxi-server:server/listen/endpoint[name='duplicate']/unix/path",
        duplicate_addr.substr(sizeof("unix:") - 1));
    CHECK_THROWS_WITH(
        sr_sess->applyChanges(), Catch::Matchers::Contains("duplicate unix listen URI"));
    CHECK(eventually_reachable(make_insecure_channel, duplicate_addr));

    // the rejecting change left the untouched original endpoint available
    CHECK(eventually_reachable(make_insecure_channel, insecure_addr));
}

TEST_CASE_METHOD(EndpointFixture, "Unix paths preserve literal percent escapes", "[endpoints]")
{
    const auto raw_addr = endpoint_addr("percent-%2f-%00-%25");
    const auto encoded_addr = endpoint_addr("percent-%252f-%2500-%2525");
    const auto socket_path = raw_addr.substr(sizeof("unix:") - 1);

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem(
        "/sysrepo-gnxi-server:server/listen/endpoint[name='escaped']/unix/path", socket_path);
    sr_sess->applyChanges();

    // percent escape text is part of the filename, not a URI instruction
    REQUIRE(std::filesystem::exists(socket_path));
#ifdef GNXI_GRPC_LEGACY_UNIX_CLIENT_URI
    // gRPC before 1.42 decodes a unix client target three times
    const auto client_addr = endpoint_addr("percent-%2525252f-%25252500-%25252525");
#else
    const auto client_addr = encoded_addr;
#endif
    REQUIRE(eventually_reachable(make_insecure_channel, client_addr));
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
