/*
 * Copyright 2025 Graphiant Inc.
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
#include <memory>

#include <grpcpp/grpcpp.h>

#include "test_main.h"

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

TEST_CASE("Top-level Set request (replace) empty", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    using namespace libyang;
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test4/params[.=\"abc\"]", "");
    sr_sess->setItem("/gnmi-server-test:test/things[name=\"B\"]/enabled", "false");
    sr_sess->applyChanges();

    auto replace = request.add_replace();

    xpath_to_path("/*", replace->mutable_path());
    replace->mutable_val()->set_json_ietf_val("{}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_REPLACE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/*"));

    auto parent = sr_sess->getData("/*");
    auto json = parent->printStr(DataFormat::JSON, PrintFlags::Siblings | PrintFlags::Shrink);
    REQUIRE_THAT(json.value(), Equals("{}"));
}

TEST_CASE("Empty leaf Set request (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/ready", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("[null]");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='A']/ready"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/ready");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals(""));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request leaflist (replace)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    using namespace libyang;
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test4/params[.=\"abc\"]", "");
    sr_sess->setItem("/gnmi-server-test:test/things[name=\"B\"]/enabled", "false");
    sr_sess->applyChanges();

    auto replace = request.add_replace();

    xpath_to_path("/gnmi-server-test:test4/params", replace->mutable_path());
    replace->mutable_val()->set_json_ietf_val("[\"speed\", \"mtu\", \"queue\"]");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_REPLACE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test4/params"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test4/params[.='mtu']");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("mtu"));

    auto parent = sr_sess->getData("/gnmi-server-test:test4/params");
    auto json = parent->printStr(DataFormat::JSON, PrintFlags::Siblings | PrintFlags::Shrink);

    CHECK_THAT(json.value(),
               Equals("{\"gnmi-server-test:test4\":{\"params\":[\"mtu\",\"queue\",\"speed\"]}}"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test4");
    sr_sess->applyChanges();
}

// TODO: replace Set replaces all of the data, not just specific leaf-list

// TEST_CASE("Top-level Set request inner leaflist (replace)", "[set]")
// {
//     ClientContext ctx;
//     SetRequest request;
//     SetResponse response;
//     using namespace libyang;
//     sr_sess->switchDatastore(sysrepo::Datastore::Running);
//     sr_sess->setItem("/gnmi-server-test:test/things[name=\"B\"]/enabled", "false");
//     sr_sess->setItem("/gnmi-server-test:test/things[name=\"A\"]/enabled", "true");
//     sr_sess->setItem("/gnmi-server-test:test/things[name=\"A\"]/amount-history", "1");
//     sr_sess->setItem("/gnmi-server-test:test/things[name=\"A\"]/amount-history", "2");
//     sr_sess->applyChanges();

//     auto replace = request.add_replace();

//     xpath_to_path("/gnmi-server-test:test/things[name=\"A\"]/amount-history",
//                   replace->mutable_path());
//     replace->mutable_val()->set_json_ietf_val("[4,5,6]");
//     auto status = client->Set(&ctx, request, &response);
//     REQUIRE(status.ok());

//     auto vals = sr_sess->getData("/gnmi-server-test:test/things[name=\"A\"]/amount-history");
//     auto json = vals->printStr(DataFormat::JSON, PrintFlags::Siblings | PrintFlags::Shrink);
//     CHECK_THAT(json.value(), Equals("{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\","
//                                     "\"amount-history\":[4,5,6]}]}}"));

//     // check that all unrelated nodes are not altered
//     auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name=\"A\"]/enabled");
//     CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));
//     val = sr_sess->getOneNode("/gnmi-server-test:test/things[name=\"B\"]/enabled");
//     CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("false"));

//     // Clean up
//     sr_sess->deleteItem("/gnmi-server-test:test");
//     sr_sess->applyChanges();
// }

TEST_CASE("Top-level Set request leaflist (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    using namespace libyang;
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test4/params[.=\"abc\"]", "");
    sr_sess->applyChanges();

    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test4/params", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("[\"speed\", \"mtu\", \"queue\"]");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test4/params"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test4/params[.='mtu']");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("mtu"));

    auto parent = sr_sess->getData("/gnmi-server-test:test4/params");
    auto json = parent->printStr(DataFormat::JSON, PrintFlags::Siblings | PrintFlags::Shrink);

    CHECK_THAT(
        json.value(),
        Equals("{\"gnmi-server-test:test4\":{\"params\":[\"abc\",\"mtu\",\"queue\",\"speed\"]}}"));
    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test4");
    sr_sess->applyChanges();
}

/* Positive Tests */
TEST_CASE("Top-level Set request (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("{\"gnmi-server-test:test\":{\"things\":[{\"name\":"
                                             "\"A\",\"enabled\":true, \"ready\":[null]}]}}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/ready");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals(""));

    // Also verify it makes it into the startup datastore since confirmed commit not used
    sr_sess->switchDatastore(sysrepo::Datastore::Startup);
    val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request (replace)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto replace = request.add_replace();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->setItem("/gnmi-server-test:test2/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/", replace->mutable_path());
    replace->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]}}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_REPLACE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Item B should have been removed since the whole config was replaced and the new config only
    // contained item A
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    // test2 should have been removed since the whole config was replaced and the new config
    // includes only the test container
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test2/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request (multi replace)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto replace = request.add_replace();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->setItem("/gnmi-server-test:test2/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/", replace->mutable_path());
    replace->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]},\"gnmi-server-"
        "test:test2\":{}}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_REPLACE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Item B should have been removed since the whole config was replaced and the new config only
    // contained item A
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    // test2 should have been removed since the whole config was replaced and the test2 container
    // was empty in the new config
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test2/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request (multi replace + update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto replace = request.add_replace();
    auto update = request.add_update();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->setItem("/gnmi-server-test:test2/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/", replace->mutable_path());
    replace->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]},\"gnmi-server-"
        "test:test2\":{\"enabled2\":true}}");
    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":false}]},\"gnmi-"
        "server-test:test2\":{\"enabled2\":false}}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_REPLACE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));
    CHECK(response.response().Get(1).op() == gnmi::UpdateResult_Operation_UPDATE);
    path = path_to_xpath(response.response().Get(1).path());
    CHECK_THAT(path, Equals("/"));

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    // The update should be applied after the replace
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("false"));

    // Item B should have been removed since the whole config was replaced and the new config only
    // contained item A
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    // The replace should have caused this node to be removed
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test2/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    // The update should be applied after the replace
    val = sr_sess->getOneNode("/gnmi-server-test:test2/enabled2");

    // The update should be applied after the replace
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("false"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->deleteItem("/gnmi-server-test:test2/enabled");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request (delete)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/", request.add_delete_());
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
}

TEST_CASE("Path-based Set request (delete)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/gnmi-server-test:test/things[name='B']/enabled", request.add_delete_());
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='B']/enabled"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='B']");
}

TEST_CASE("Path-based Set request (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("{\"enabled\":true}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='A']"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Leaf Set request (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/name", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("\"A\"");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='A']/name"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/name");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("A"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Set request (with prefix)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']", request.mutable_prefix());
    xpath_to_path("/name", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("\"A\"");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    auto prefix = path_to_xpath(response.prefix());
    CHECK_THAT(prefix, Equals("/gnmi-server-test:test/things[name='A']"));
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/name"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/name");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("A"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Set request (with empty prefix)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    // Add empty prefix
    request.mutable_prefix();
    xpath_to_path("/gnmi-server-test:test/things[name='A']/name", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("\"A\"");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    auto prefix = path_to_xpath(response.prefix());
    CHECK_THAT(prefix, Equals("/"));
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='A']/name"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/name");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("A"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Set request transaction (update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]}}");
    update = request.add_update();
    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"B\",\"enabled\":true}]}}");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));
    CHECK(response.response().Get(1).op() == gnmi::UpdateResult_Operation_UPDATE);
    path = path_to_xpath(response.response().Get(1).path());
    CHECK_THAT(path, Equals("/"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));
    val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='B']");
    sr_sess->applyChanges();
}

TEST_CASE("Set request transaction (delete+update)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/gnmi-server-test:test/things[name='B']", request.add_delete_());
    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]}}");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='B']"));
    CHECK(response.response().Get(1).op() == gnmi::UpdateResult_Operation_UPDATE);
    path = path_to_xpath(response.response().Get(1).path());
    CHECK_THAT(path, Equals("/"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);

    // TODO: try catch block
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']"),
                      Contains("SR_ERR_NOT_FOUND"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->applyChanges();
}

TEST_CASE("Set request (delete with wildcards)", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='A']/enabled", "true");
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/gnmi-server-test:test/*/enabled", request.add_delete_());
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/*/enabled"));

    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='B']");
}

// gNMI spec §3.4.6: In the case that a path specifies an element within the data tree that does not
// exist, these deletes MUST be silently accepted.
TEST_CASE("Set request (delete) with non-existent path", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    xpath_to_path("/gnmi-server-test:test-state/things[name='not-found']", request.add_delete_());

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::OK);

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='not-found']"));
}

TEST_CASE("Set request (delete) the same path", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='foo']/enabled", "true");
    sr_sess->applyChanges();

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    xpath_to_path("/gnmi-server-test:test/things[name='foo']", request.add_delete_());
    xpath_to_path("/gnmi-server-test:test/things[name='foo']", request.add_delete_());

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::OK);

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='foo']"));

    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
}

TEST_CASE("Set request (delete) child then parent", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='foo']/enabled", "true");
    sr_sess->applyChanges();

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    xpath_to_path("/gnmi-server-test:test/things[name='foo']/enabled", request.add_delete_());
    xpath_to_path("/gnmi-server-test:test/things[name='foo']", request.add_delete_());

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::OK);

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    CHECK_THAT(path_to_xpath(response.response().Get(0).path()),
               Equals("/gnmi-server-test:test/things[name='foo']/enabled"));
    CHECK_THAT(path_to_xpath(response.response().Get(1).path()),
               Equals("/gnmi-server-test:test/things[name='foo']"));

    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
}

TEST_CASE("Set request (delete) parent then child", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='foo']/enabled", "true");
    sr_sess->applyChanges();

    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    xpath_to_path("/gnmi-server-test:test/things[name='foo']", request.add_delete_());
    xpath_to_path("/gnmi-server-test:test/things[name='foo']/enabled", request.add_delete_());

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::OK);

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 2);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_DELETE);
    CHECK_THAT(path_to_xpath(response.response().Get(0).path()),
               Equals("/gnmi-server-test:test/things[name='foo']"));
    CHECK_THAT(path_to_xpath(response.response().Get(1).path()),
               Equals("/gnmi-server-test:test/things[name='foo']/enabled"));

    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='foo']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
}

TEST_CASE("Set request for list with composite key", "[set]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    CHECK_THROWS_WITH(
        sr_sess->getOneNode("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data"),
        Contains("SR_ERR_NOT_FOUND"));

    xpath_to_path("/gnmi-server-test:test3/complex-list[name='foo'][type='bar']/data",
                  update->mutable_path());

    // check parsing of the xpath_to_path() function
    auto reqpath = update->path();
    CHECK(reqpath.elem_size() == 3);
    CHECK_THAT(reqpath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(reqpath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(reqpath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(reqpath.elem(1).key().at("type"), Equals("bar"));
    CHECK_THAT(reqpath.elem(2).name(), Equals("data"));

    update->mutable_val()->set_json_ietf_val("\"baz\"");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.ok());

    auto val =
        sr_sess->getOneNode("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data");
    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("baz"));

    // cleanup
    sr_sess->deleteItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']");
    sr_sess->applyChanges();
}

/* Scale Tests */
std::string generate_random_string(size_t length)
{
    std::string str;
    str.reserve(length + 2); // Reserve space for the string to avoid reallocations

    str += '"';
    for (size_t i = 0; i < length; ++i)
    {
        str += 'a' + rand() % 26;
    }
    str += '"';

    return str;
}

TEST_CASE("Scaled Set request (update)", "[set-scale]")
{
    using namespace libyang;
    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    ScaleTestLogLevelReducer _log_reducer;

    for (int i = 0; i < 100; i++)
    {
        grpc::ClientContext ctx;
        gnmi::SetRequest request;
        gnmi::SetResponse response;
        xpath_to_path("/", request.add_delete_());

        for (int j = 0; j < 100; j++)
        {
            auto update = request.add_update();
            auto xpath =
                "/gnmi-server-test:test/things[name=\"A" + std::to_string(j) + "\"]/description";

            xpath_to_path(xpath, update->mutable_path());
            update->mutable_val()->set_json_ietf_val(generate_random_string(1000));
        }
        auto status = client->Set(&ctx, request, &response);
        REQUIRE(status.ok());
    }

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things");
    sr_sess->applyChanges();

    auto parent = sr_sess->getData("/*");
    auto json = parent->printStr(DataFormat::JSON, PrintFlags::Siblings | PrintFlags::Shrink);
    CHECK_THAT(json.value(), Equals("{}"));
}

/* Negative Tests */

TEST_CASE("Set request (no val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val();
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Value not set"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (ascii val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_ascii_val("true");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported ASCII Encoding"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (JSON val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_json_val("true");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported JSON Encoding"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (bytes val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_bytes_val("1");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf bytes type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (proto-bytes val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_proto_bytes("1");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported PROTOBUF BYTE Encoding"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (any-val val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->mutable_any_val()->set_value("1");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported PROTOBUF Encoding"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (leaf-list val type)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->mutable_leaflist_val()->add_element()->set_string_val("true");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf leaflist type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (bool val)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_bool_val(true);
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf bool type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (string val)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/description", update->mutable_path());
    update->mutable_val()->set_string_val("This is item A");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf string type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (uint val)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/amount", update->mutable_path());
    update->mutable_val()->set_uint_val(42);
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf uint type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (int val)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/signed-amount", update->mutable_path());
    update->mutable_val()->set_int_val(-42);
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf int type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (double val)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/decimal-amount", update->mutable_path());
    update->mutable_val()->set_double_val(42.1);
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unsupported protobuf double type"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (no path)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    update->mutable_val();
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Update no path or value"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

TEST_CASE("Set request (incorrect prefix)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']", update->mutable_path());
    // The prefix, if any, should be gnmi-server-test
    update->mutable_val()->set_json_ietf_val("{\"gnmi-server-test-wine:enabled\":true}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse value fragment data: LY_EVALID"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);
}

// Check that failing transaction doesn't modify state
TEST_CASE("Set request failing transaction (2 updates)", "[set-neg]")
{
    grpc::ClientContext ctx;
    grpc::ClientContext ctx2;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]}}");
    update = request.add_update();
    xpath_to_path("/", update->mutable_path());
    // Contains bad value for enabled so expected to fail
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"B\",\"enabled\":\"maybe\"}]}}");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse data: LY_EVALID"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    // Check that no changes happened to the state
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']"),
                      Contains("SR_ERR_NOT_FOUND"));

    request.Clear();
    // Try again with an unrelated update that succeeds
    update = request.add_update();
    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"C\",\"enabled\":true}]}}");
    status = client->Set(&ctx2, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    // Check that no state was changed other than that related to the most recent updae
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']"),
                      Contains("SR_ERR_NOT_FOUND"));
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='C']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='C']");
    sr_sess->applyChanges();
}

// Check that failing transaction doesn't modify state
TEST_CASE("Set request failing transaction (delete+update)", "[set-neg]")
{
    grpc::ClientContext ctx;
    grpc::ClientContext ctx2;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    sr_sess->setItem("/gnmi-server-test:test/things[name='B']/enabled", "true");
    sr_sess->applyChanges();

    xpath_to_path("/gnmi-server-test:test/things[name='B']", request.add_delete_());
    xpath_to_path("/", update->mutable_path());
    // Contains bad value for enabled so expected to fail
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"A\",\"enabled\":\"maybe\"}]}}");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse data: LY_EVALID"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);

    // Check that no changes happened to the state
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));

    request.Clear();
    // Try again with an unrelated update that succeeds
    update = request.add_update();
    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"gnmi-server-test:test\":{\"things\":[{\"name\":\"C\",\"enabled\":true}]}}");
    status = client->Set(&ctx2, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/"));

    // Check that no state was changed other than that related to the most recent updae
    val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']"),
                      Contains("SR_ERR_NOT_FOUND"));
    val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='C']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='B']");
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='C']");
    sr_sess->applyChanges();
}

// Check that failing transaction doesn't modify state
TEST_CASE("Set request failing transaction (2 leaf updates)", "[set-neg]")
{
    grpc::ClientContext ctx;
    grpc::ClientContext ctx2;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/things[name='A']/enabled", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("true");
    update = request.add_update();
    xpath_to_path("/gnmi-server-test:test/things[name='B']/enabled", update->mutable_path());
    // Contains bad value for enabled so expected to fail
    update->mutable_val()->set_json_ietf_val("\"maybe\"");

    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse value fragment data: LY_EVALID"));

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() == 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 0);

    sr_sess->switchDatastore(sysrepo::Datastore::Running);

    // Check that no changes happened to the state
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']"),
                      Contains("SR_ERR_NOT_FOUND"));

    request.Clear();
    // Try again with an unrelated update that succeeds
    update = request.add_update();
    xpath_to_path("/gnmi-server-test:test/things[name='C']/enabled", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("true");
    status = client->Set(&ctx2, request, &response);
    CHECK(status.ok());

    CHECK(response.extension_size() == 0);
    CHECK(response.timestamp() > 0);
    CHECK(!response.has_prefix());
    REQUIRE(response.response_size() == 1);
    CHECK(response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    auto path = path_to_xpath(response.response().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test/things[name='C']/enabled"));

    // Check that no state was changed other than that related to the most recent updae
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']"),
                      Contains("SR_ERR_NOT_FOUND"));
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='B']"),
                      Contains("SR_ERR_NOT_FOUND"));
    auto val = sr_sess->getOneNode("/gnmi-server-test:test/things[name='C']/enabled");

    CHECK_THAT(std::string(val.asTerm().valueStr()), Equals("true"));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test/things[name='C']");
    sr_sess->applyChanges();
}

TEST_CASE("Top-level Set request (update, no namespace)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/", update->mutable_path());
    update->mutable_val()->set_json_ietf_val(
        "{\"test\":{\"things\":[{\"name\":\"A\",\"enabled\":true}]}}");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse data: LY_EVALID"));

    sr_sess->switchDatastore(sysrepo::Datastore::Running);
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test/things[name='A']/enabled"),
                      Contains("SR_ERR_NOT_FOUND"));
}

TEST_CASE("Set request (update with wildcards)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test/*/enabled", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("true");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Can't parse value fragment data: LY_EVALID"));
}

TEST_CASE("Set request (application error string)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test2/custom-error", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("1");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK_THAT(status.error_message(),
               Equals("Fiddlesticks: /gnmi-server-test:test2/custom-error"));
}

TEST_CASE("Set request (data model error)", "[set-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SetRequest request;
    gnmi::SetResponse response;
    auto update = request.add_update();

    xpath_to_path("/gnmi-server-test:test2/must-error", update->mutable_path());
    update->mutable_val()->set_json_ietf_val("1");
    auto status = client->Set(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK_THAT(status.error_message(), Equals("Must condition \"current() > 42\" not satisfied. "
                                              "(path \"/gnmi-server-test:test2/must-error\")"));
}
