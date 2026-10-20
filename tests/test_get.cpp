/**
 * @file test_get.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Get RPC tests
 *
 * @copyright
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

#include <unistd.h>

#include "catch2/catch.hpp"
#include <memory>
#include <set>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "test_main.h"

using namespace std;
using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

/* Positive Tests */

// A path referring to "root" (which is represented by a path consisting of an empty set of
// elements) should result in the nodes childA and childB and all of their children ... being
// considered by the relevant operation.
TEST_CASE("Top-level Get request", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;
    bool found = false;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());
    std::cout << __func__ << ":" << __LINE__ << std::endl;

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    for (auto it : response.notification().Get(0).update())
    {
        auto path = path_to_xpath(it.path());
        std::cout << "found path: " << path << std::endl;
        if (path.compare("/sysrepo-gnxi-test:test-state"))
        {
            continue;
        }
        found = true;
        CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
              gnmi::TypedValue::ValueCase::kJsonIetfVal);
        CHECK(!response.notification().Get(0).atomic());
    }
    CHECK(found);
}

static void single_get()
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;
    bool found = false;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    for (auto it : response.notification().Get(0).update())
    {
        auto path = path_to_xpath(it.path());
        if (path.compare("/sysrepo-gnxi-test:test-state"))
        {
            continue;
        }
        found = true;
        CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
              gnmi::TypedValue::ValueCase::kJsonIetfVal);
        CHECK(!response.notification().Get(0).atomic());
    }
    CHECK(found);
}

TEST_CASE("Top-level multiple parallel Get requests", "[get]")
{
#define NUM_THREADS 20
    std::thread threads[NUM_THREADS];
    for (auto i = 0; i < NUM_THREADS; i++)
    {
        threads[i] = std::thread(single_get);
    }
    for (auto i = 0; i < NUM_THREADS; i++)
    {
        threads[i].join();
    }
}

TEST_CASE("Get request of all module oper state", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto data = response.notification().Get(0).update();
    auto json = data.Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request of one list item oper state", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"A\",\"counter\":\"1\"}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']"));
    CHECK(!response.notification().Get(0).atomic());
}

// gNMI spec reference:
// In the case that the data item at the specified path is a leaf node (i.e., has no children, and
// an associated value) the value of that leaf is encoded directly - i.e., the "bare" value is
// specified (i.e., a JSON object is not CHECKd, and a bare JSON value is included).
TEST_CASE("Get request of one leaf of oper state", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']/counter", request.add_path());
    auto status = gnmi_client->Get(&ctx, request, &response);

    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    // This is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(json, Equals("\"1\""));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with prefix", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']", request.mutable_prefix(), "");
    xpath_to_path("/counter", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    auto prefix = path_to_xpath(response.notification().Get(0).prefix());
    CHECK_THAT(prefix, Equals("/"));
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    // This is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(json, Equals("\"1\""));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with target", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    *request.mutable_prefix()->mutable_target() = "foo";
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(response.notification().Get(0).has_prefix());
    CHECK_THAT(response.notification().Get(0).prefix().target(), Equals("foo"));
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"A\",\"counter\":\"1\"}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request for config datastore", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_type(gnmi::GetRequest_DataType::GetRequest_DataType_CONFIG);
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test/things[name='A']/enabled", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    CHECK(!response.notification().Get(0).atomic());
    REQUIRE(response.notification().Get(0).update_size() == 0);

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/sysrepo-gnxi-test:test/things[name='A']/enabled", "true");
    sr_sess->applyChanges();

    grpc::ClientContext ctx2;
    status = gnmi_client->Get(&ctx2, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("true"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test/things[name='A']/enabled"));
    CHECK(!response.notification().Get(0).atomic());

    sr_sess->deleteItem("/sysrepo-gnxi-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request with wildcard", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/*/counter", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 2);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    // This is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(json, Equals("\"1\""));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(response.notification().Get(0).update().Get(1).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.notification().Get(0).update().Get(1).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"2\""));
    path = path_to_xpath(response.notification().Get(0).update().Get(1).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='B']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with multiple paths", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']/counter", request.add_path());
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='B']/counter", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 2);

    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    // This is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(json, Equals("\"1\""));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());

    CHECK(response.notification().Get(1).delete__size() == 0);
    CHECK(response.notification().Get(1).timestamp() > 0);
    CHECK(!response.notification().Get(1).has_prefix());
    REQUIRE(response.notification().Get(1).update_size() == 1);
    CHECK(response.notification().Get(1).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.notification().Get(1).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"2\""));
    path = path_to_xpath(response.notification().Get(1).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='B']/counter"));
    CHECK(!response.notification().Get(1).atomic());
}

TEST_CASE("Get request of empty container", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/cargo", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/cargo"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request of list container", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    CHECK(!response.notification().Get(0).atomic());
    REQUIRE(response.notification().Get(0).update_size() == 2);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"A\",\"counter\":\"1\"}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='A']"));
    CHECK(response.notification().Get(0).update().Get(1).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.notification().Get(0).update().Get(1).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"B\",\"counter\":\"2\"}"));
    path = path_to_xpath(response.notification().Get(0).update().Get(1).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='B']"));
}

TEST_CASE("Get request with non-existent path", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='not-found']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());
    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    CHECK(!response.notification().Get(0).atomic());
    REQUIRE(response.notification().Get(0).update_size() == 0);
}

TEST_CASE("Get request of one list item where name contains /", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']", std::nullopt);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']/counter", "5");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"Gigabit5/0/0\",\"counter\":\"5\"}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request of one leaf where parent name contains /", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(
        "/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']/counter", request.add_path());

    const gnmi::Path reqpath = request.path(0);
    CHECK(reqpath.elem_size() == 3);
    CHECK_THAT(reqpath.elem(0).name(), Equals("sysrepo-gnxi-test:test-state"));
    CHECK_THAT(reqpath.elem(1).name(), Equals("things"));
    CHECK_THAT(reqpath.elem(1).key().at("name"), Equals("Gigabit5/0/0"));
    CHECK_THAT(reqpath.elem(2).name(), Equals("counter"));

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']", std::nullopt);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']/counter", "5");
    sr_sess->applyChanges();

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"5\""));
    CHECK(!response.notification().Get(0).atomic());

    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 3);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test-state"));
    CHECK_THAT(resppath.elem(1).name(), Equals("things"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("Gigabit5/0/0"));
    CHECK_THAT(resppath.elem(2).name(), Equals("counter"));

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='Gigabit5/0/0']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request of one list item where name contains [", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='One[1]']", std::nullopt);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='One[1]']/counter", "7");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='One[1]']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"One[1]\",\"counter\":\"7\"}"));
    auto path = path_to_xpath(response.notification().Get(0).update().Get(0).path());
    CHECK_THAT(path, Equals("/sysrepo-gnxi-test:test-state/things[name='One[1]']"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='One[1]']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request of one list item where name contains '", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name=\"to-cpe2'\"]", std::nullopt);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name=\"to-cpe2'\"]/counter", "8");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name=\"to-cpe2'\"]", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"to-cpe2'\",\"counter\":\"8\"}"));
    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test-state"));
    CHECK_THAT(resppath.elem(1).name(), Equals("things"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("to-cpe2'"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name=\"to-cpe2'\"]");
    sr_sess->applyChanges();
}

TEST_CASE("Get request of one list item where name contains \\", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='to\\cpe2']", std::nullopt);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='to\\cpe2']/counter", "8");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='to\\cpe2']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"to\\\\cpe2\",\"counter\":\"8\"}"));
    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test-state"));
    CHECK_THAT(resppath.elem(1).name(), Equals("things"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("to\\cpe2"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='to\\cpe2']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request from list with composite key ", "[get-composite-key]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    // create an entry in the list wiht composite keys
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test3/complex-list[type='bar'][name='foo']/data", "baz");
    sr_sess->applyChanges();

    // prepare a get request for that entry
    xpath_to_path(
        "/sysrepo-gnxi-test:test3/complex-list[name='foo'][type='bar']", request.add_path());

    // check parsing of the xpath_to_path() function
    const gnmi::Path reqpath = request.path(0);
    CHECK(reqpath.elem_size() == 2);
    CHECK_THAT(reqpath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(reqpath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(reqpath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(reqpath.elem(1).key().at("type"), Equals("bar"));

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\"}"));
    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem("/sysrepo-gnxi-test:test3/complex-list[type='bar'][name='foo']");
    sr_sess->applyChanges();
}

TEST_CASE(
    "Get request from list with composite key having slashes ", "[get-composite-key-with-slashes]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    // create an entry in the list wiht composite keys
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem(
        "/sysrepo-gnxi-test:test3/complex-list[type='bar/cat/fish'][name='foo/dog/sausage']/data",
        "baz");
    sr_sess->applyChanges();

    // prepare a get request for that entry
    xpath_to_path(
        "/sysrepo-gnxi-test:test3/complex-list[name='foo/dog/sausage'][type='bar/cat/fish']",
        request.add_path());

    // check parsing of the xpath_to_path() function
    const gnmi::Path reqpath = request.path(0);
    CHECK(reqpath.elem_size() == 2);
    CHECK_THAT(reqpath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(reqpath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(reqpath.elem(1).key().at("name"), Equals("foo/dog/sausage"));
    CHECK_THAT(reqpath.elem(1).key().at("type"), Equals("bar/cat/fish"));

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(
        json, Equals("{\"type\":\"bar/cat/fish\",\"name\":\"foo/dog/sausage\",\"data\":\"baz\"}"));
    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo/dog/sausage"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar/cat/fish"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem(
        "/sysrepo-gnxi-test:test3/complex-list[type='bar/cat/fish'][name='foo/dog/sausage']");
    sr_sess->applyChanges();
}

TEST_CASE("Get request from list with composite key having doube-quotes(\") ",
    "[get-composite-key-with-quotes]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    // create an entry in the list with composite keys
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test3/complex-list[type='bar/cat/fish'][name='quotes: \", "
                     "blah blah']/data",
        "baz");
    sr_sess->applyChanges();

    // prepare a get request for that entry
    xpath_to_path(
        "/sysrepo-gnxi-test:test3/complex-list[name='quotes: \", blah blah'][type='bar/cat/fish']",
        request.add_path());

    // check parsing of the xpath_to_path() function
    const gnmi::Path reqpath = request.path(0);
    CHECK(reqpath.elem_size() == 2);
    CHECK_THAT(reqpath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(reqpath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(reqpath.elem(1).key().at("name"), Equals("quotes: \", blah blah"));
    CHECK_THAT(reqpath.elem(1).key().at("type"), Equals("bar/cat/fish"));

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    CHECK(response.notification().Get(0).update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.notification().Get(0).update().Get(0).val().json_ietf_val();
    CHECK_THAT(json,
        Equals(
            "{\"type\":\"bar/cat/fish\",\"name\":\"quotes: \\\", blah blah\",\"data\":\"baz\"}"));
    auto resppath = response.notification().Get(0).update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("sysrepo-gnxi-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("quotes: \", blah blah"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar/cat/fish"));
    CHECK(!response.notification().Get(0).atomic());

    // cleanup
    sr_sess->deleteItem(
        "/sysrepo-gnxi-test:test3/complex-list[type='bar/cat/fish'][name='quotes: \", blah blah']");
    sr_sess->applyChanges();
}

/* Negative tests */

TEST_CASE("Get request with unsupported encoding type", "[get-neg]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::BYTES);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("BYTES"));
}

TEST_CASE("Get request with simple JSON encoding", "[get-neg]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("JSON"));
}

TEST_CASE("Get request with unsupported use of models", "[get-neg]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    auto model_data = request.add_use_models();
    model_data->set_name("sysrepo-gnxi-test");
    model_data->set_version("2021-02-10");
    model_data->set_organization("graphiant");
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("use_model feature unsupported"));
}

TEST_CASE("Get request with invalid datatype", "[get-neg]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_type(static_cast<gnmi::GetRequest_DataType>(42));
    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals(""));
}

TEST_CASE("Get request with relative path", "[get-neg]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/cargo/../things[name='A']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(
        status.error_message(), Equals("gNMI paths MUST be absolute, \"..\" is not allowed"));
}

TEST_CASE("Get request of one list item where name contains \"", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/sysrepo-gnxi-test:test-state/things[name='to-cpe1\"']/counter", "6");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='to-cpe1\"']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    // the double quote is preserved in the value, escaped as per JSON (RFC 8259 §7)
    CHECK_THAT(
        update.val().json_ietf_val(), Equals("{\"name\":\"to-cpe1\\\"\",\"counter\":\"6\"}"));
    CHECK_THAT(path_to_xpath(update.path()),
        Equals("/sysrepo-gnxi-test:test-state/things[name='to-cpe1\"']"));
    CHECK(!response.notification().Get(0).atomic());

    // clean up
    sr_sess->deleteItem("/sysrepo-gnxi-test:test-state/things[name='to-cpe1\"']");
    sr_sess->applyChanges();
}

// origin tests

TEST_CASE("Get request with default origin (bare names)", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/test-state/things[name='A']/counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    // this is a 64-bit value, so it is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(update.val().json_ietf_val(), Equals("\"1\""));
    CHECK_THAT(update.path().origin(), Equals("openconfig"));
    CHECK_THAT(path_to_xpath(update.path()), Equals("/test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with openconfig origin and qualified names", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path(
        "/sysrepo-gnxi-test:test-state/things[name='A']/counter", request.add_path(), "openconfig");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    CHECK_THAT(update.val().json_ietf_val(), Equals("\"1\""));
    CHECK_THAT(update.path().origin(), Equals("openconfig"));
    CHECK_THAT(path_to_xpath(update.path()), Equals("/test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with rfc7951 origin and bare names", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/test-state/things[name='A']/counter", request.add_path(), "rfc7951");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    CHECK_THAT(update.val().json_ietf_val(), Equals("\"1\""));
    CHECK_THAT(update.path().origin(), Equals("rfc7951"));
    CHECK_THAT(path_to_xpath(update.path()),
        Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with unsupported origin", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.add_path(), "frob");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("unsupported origin \"frob\""));
}

TEST_CASE("Get request with origin in both prefix and path", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state", request.mutable_prefix());
    xpath_to_path("/things[name='A']", request.add_path());

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(),
        Equals("origin must not be specified in both prefix and path (gNMI spec 2.7)"));
}

TEST_CASE("Get request with origin and target in prefix", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='A']", request.mutable_prefix(), "");
    request.mutable_prefix()->set_origin("rfc7951");
    request.mutable_prefix()->set_target("example");
    xpath_to_path("/counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    REQUIRE(response.notification().Get(0).has_prefix());
    CHECK_THAT(response.notification().Get(0).prefix().target(), Equals("example"));
    CHECK_THAT(response.notification().Get(0).prefix().origin(), Equals("rfc7951"));
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    // this is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(update.val().json_ietf_val(), Equals("\"1\""));
    CHECK_THAT(update.path().origin(), Equals("rfc7951"));
    CHECK_THAT(path_to_xpath(update.path()),
        Equals("/sysrepo-gnxi-test:test-state/things[name='A']/counter"));
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with paths of different origins", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem(
        "/sysrepo-gnxi-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']/score", "98");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']/score",
        request.add_path());
    xpath_to_path("/test-state/things[name='A']/counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 2);

    // each notification carries the origin and naming convention of its path
    auto update = response.notification().Get(0).update().Get(0);
    CHECK_THAT(update.path().origin(), Equals("rfc7951"));
    CHECK_THAT(path_to_xpath(update.path()),
        Equals("/sysrepo-gnxi-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']"
               "/score"));
    CHECK_THAT(update.val().json_ietf_val(), Equals("98"));

    update = response.notification().Get(1).update().Get(0);
    CHECK_THAT(update.path().origin(), Equals("openconfig"));
    CHECK_THAT(path_to_xpath(update.path()), Equals("/test-state/things[name='A']/counter"));
    // this is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(update.val().json_ietf_val(), Equals("\"1\""));

    // clean up
    sr_sess->deleteItem("/sysrepo-gnxi-test-wine:wines");
    sr_sess->applyChanges();
}

TEST_CASE("Get request with relative element in prefix", "[get]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/..", request.mutable_prefix(), "");
    xpath_to_path("/", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(
        status.error_message(), Equals("gNMI paths MUST be absolute, \"..\" is not allowed"));
}

TEST_CASE("Get request with ambiguous bare element name", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/test2", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Contains("ambiguous path element \"test2\""));
    CHECK_THAT(status.error_message(), Contains("sysrepo-gnxi-test:test2"));
    CHECK_THAT(status.error_message(), Contains("sysrepo-gnxi-test-clash:test2"));
}

TEST_CASE("Get request with disambiguated qualified element name", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test-clash:test2", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 1);
    auto update = response.notification().Get(0).update().Get(0);
    CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
    CHECK_THAT(update.val().json_ietf_val(), Equals("{}"));
    CHECK_THAT(update.path().origin(), Equals("openconfig"));
    REQUIRE(update.path().elem_size() == 1);
    CHECK_THAT(update.path().elem(0).name(), Equals("test2"));
}

TEST_CASE("Get request with unknown bare element name", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/frobnicate", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("unknown path element \"frobnicate\""));
}

TEST_CASE("Get request with unknown module", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/frob:node", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("unknown module \"frob\""));
}

TEST_CASE("Get request with multi-level wildcard", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/test-state/.../counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 2);
    std::set<std::string> counters;
    std::set<std::string> paths;
    for (const auto &update : response.notification().Get(0).update())
    {
        CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
        counters.insert(update.val().json_ietf_val());
        CHECK_THAT(update.path().origin(), Equals("openconfig"));
        paths.insert(path_to_xpath(update.path()));
    }
    // this is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK(counters == std::set<std::string>{"\"1\"", "\"2\""});
    CHECK(paths == std::set<std::string>{"/test-state/things[name='A']/counter",
                       "/test-state/things[name='B']/counter"});
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with wildcard key value", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='*']/counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    REQUIRE(response.notification().Get(0).update_size() == 2);
    std::set<std::string> counters;
    std::set<std::string> paths;
    for (const auto &update : response.notification().Get(0).update())
    {
        CHECK(update.val().value_case() == gnmi::TypedValue::ValueCase::kJsonIetfVal);
        counters.insert(update.val().json_ietf_val());
        CHECK_THAT(update.path().origin(), Equals("openconfig"));
        paths.insert(path_to_xpath(update.path()));
    }
    // this is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK(counters == std::set<std::string>{"\"1\"", "\"2\""});
    CHECK(paths == std::set<std::string>{"/test-state/things[name='A']/counter",
                       "/test-state/things[name='B']/counter"});
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with wildcard in prefix", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test:test-state/things[name='*']", request.mutable_prefix());
    xpath_to_path("/counter", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    REQUIRE(response.notification().Get(0).update_size() == 2);
    std::set<std::string> counters;
    std::set<std::string> paths;
    for (const auto &update : response.notification().Get(0).update())
    {
        counters.insert(update.val().json_ietf_val());
        CHECK_THAT(update.path().origin(), Equals("rfc7951"));
        paths.insert(path_to_xpath(update.path()));
    }
    // this is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK(counters == std::set<std::string>{"\"1\"", "\"2\""});
    CHECK(paths == std::set<std::string>{"/sysrepo-gnxi-test:test-state/things[name='A']/counter",
                       "/sysrepo-gnxi-test:test-state/things[name='B']/counter"});
    CHECK(!response.notification().Get(0).atomic());
}

TEST_CASE("Get request with key value containing both quote characters", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    // the key value cannot be expressed in XPath (it contains both quote
    // characters), so it is set directly
    auto path = request.add_path();
    xpath_to_path("/test-state/things", path, "");
    (*path->mutable_elem(1)->mutable_key())["name"] = "a'b\"c";

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(),
        Equals("key \"name\" value contains both quote characters: a'b\"c"));
}

TEST_CASE("Get request from a module under a different origin", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/sysrepo-gnxi-test-wine:wines", request.add_path(), "openconfig");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(),
        Equals("module \"sysrepo-gnxi-test-wine\" is under origin \"rfc7951\" and cannot "
               "be addressed under origin \"openconfig\""));
}

TEST_CASE("Get request with bare name of a module under a different origin", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/wines", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("unknown path element \"wines\""));
}

TEST_CASE("Top-level Get request excludes data of other origins", "[get-origin]")
{
    grpc::ClientContext ctx;
    gnmi::GetRequest request;
    gnmi::GetResponse response;
    bool found = false;

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem(
        "/sysrepo-gnxi-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']/score", "98");
    sr_sess->applyChanges();

    request.set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/", request.add_path(), "");

    auto status = gnmi_client->Get(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    REQUIRE(response.notification_size() == 1);
    CHECK(response.notification().Get(0).delete__size() == 0);
    CHECK(response.notification().Get(0).timestamp() > 0);
    CHECK(!response.notification().Get(0).has_prefix());
    for (const auto &update : response.notification().Get(0).update())
    {
        CHECK_THAT(update.path().origin(), Equals("openconfig"));
        auto path = path_to_xpath(update.path());
        CHECK(path.rfind("/wines", 0) != 0);
        if (path.rfind("/test-state", 0) == 0)
        {
            found = true;
        }
    }
    CHECK(found);
    CHECK(!response.notification().Get(0).atomic());

    // clean up
    sr_sess->deleteItem("/sysrepo-gnxi-test-wine:wines");
    sr_sess->applyChanges();
}
