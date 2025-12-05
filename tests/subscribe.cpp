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
#include <chrono>
#include <memory>
#include <thread>

#include "main.h"
#include <grpcpp/grpcpp.h>
#include <sysrepo-cpp/Connection.hpp>

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

/* Positive Tests */
TEST_CASE("Subscribe (once)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == true);

    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    success = rw->WritesDone();
    CHECK(success == true);
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe (poll)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Write(poll_request);
    CHECK(success == true);
    success = rw->WritesDone();
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe (stream-sample)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    std::chrono::seconds interval(1);

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::SAMPLE);
    sub->set_sample_interval(std::chrono::nanoseconds(interval).count());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Wait for one update after the initial one
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    // And then cancel
    ctx.TryCancel();
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
}

TEST_CASE("Subscribe (once) with prefix", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", list->mutable_prefix());
    xpath_to_path("/things[name='A']", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == true);

    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    auto prefix = path_to_xpath(response.update().prefix());
    CHECK_THAT(prefix, Equals("/"));
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"A\",\"counter\":\"1\"}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='A']"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    success = rw->WritesDone();
    CHECK(success == true);
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe (once) with target", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    *list->mutable_prefix()->mutable_target() = "foo";
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == true);

    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(response.update().has_prefix());
    CHECK_THAT(response.update().prefix().target(), Equals("foo"));
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"things\":[{\"name\":\"A\",\"counter\":\"1\"},{\"name\":\"B\","
                            "\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    success = rw->WritesDone();
    CHECK(success == true);
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe (once) with wildcards", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/*/counter", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == true);

    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 2);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    // This is a 64-bit value, so is represented as a string (ref: RFC7951 §6.1)
    CHECK_THAT(json, Equals("\"1\""));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='A']/counter"));
    CHECK(response.update().update().Get(1).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(1).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"2\""));
    path = path_to_xpath(response.update().update().Get(1).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='B']/counter"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    success = rw->WritesDone();
    CHECK(success == true);
    auto status = rw->Finish();
    CHECK(status.ok());
}

static void onchange_cancel_thread(grpc::ClientContext &ctx)
{
    // Wait long enough to be sure that no pending message would have been sent
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ctx.TryCancel();
}

TEST_CASE("Subscribe (on-change, no updates)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    /* Initial update */
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(
        json,
        Equals("{\"things\":[{\"@\":{\"yang:key\":[null]},\"name\":\"A\",\"counter\":\"1\"},{\"@\":"
               "{\"yang:key\":\"[name='A']\"},\"name\":\"B\",\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    auto cancel_thread = std::thread(onchange_cancel_thread, std::ref(ctx));

    success = rw->Read(&response);
    CHECK(success == false);

    cancel_thread.join();

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));
}

TEST_CASE("Subscribe (on-change, with update)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    // Make sure the item doesn't already exist, or no notifications
    // will be generated and the test will hang
    CHECK_THROWS_WITH(sr_sess->getOneNode("/gnmi-server-test:test-state/things[name='C']/name"),
                      Contains("SR_ERR_NOT_FOUND"));

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(
        json,
        Equals("{\"things\":[{\"@\":{\"yang:key\":[null]},\"name\":\"A\",\"counter\":\"1\"},{\"@\":"
               "{\"yang:key\":\"[name='A']\"},\"name\":\"B\",\"counter\":\"2\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "3");
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter2", "23");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"name\":\"C\",\"counter\":\"3\",\"counter2\":\"23\"}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']"));
    CHECK(!response.update().atomic());

    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "4");
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter2", "24");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    REQUIRE(response.update().delete__size() == 2);
    path = path_to_xpath(response.update().delete_().Get(0));
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    path = path_to_xpath(response.update().delete_().Get(1));
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter2"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 2);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"4\",\"@counter\":{\"yang:operation\":\"replace\",\"yang:orig-"
                            "default\":false,\"yang:orig-value\":\"3\"}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(response.update().update().Get(1).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(1).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"24\",\"@counter2\":{\"yang:operation\":\"replace\",\"yang:orig-"
                            "default\":false,\"yang:orig-value\":\"23\"}"));
    path = path_to_xpath(response.update().update().Get(1).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter2"));
    CHECK(!response.update().atomic());

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
    sr_sess->applyChanges();
}

TEST_CASE("Subscribe (on-change, with delete)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "4");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(
        json,
        Equals("{\"things\":[{\"@\":{\"yang:key\":[null]},\"name\":\"A\",\"counter\":\"1\"},{\"@\":"
               "{\"yang:key\":\"[name='A']\"},\"name\":\"B\",\"counter\":\"2\"},{\"@\":{\"yang:"
               "key\":\"[name='B']\"},\"name\":\"C\",\"counter\":\"4\"}],\"cargo\":{}}"));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().update_size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().delete__size() == 1);
    path = path_to_xpath(response.update().delete_().Get(0));
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']"));
    CHECK(!response.update().atomic());

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));
}

TEST_CASE("Subscribe for leaf (on-change, update)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "5");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']/counter", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"5\""));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "6");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);

    REQUIRE(response.update().delete__size() == 1);
    path = path_to_xpath(response.update().delete_().Get(0));
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"6\",\"@counter\":{\"yang:operation\":\"replace\",\"yang:orig-"
                            "default\":false,\"yang:orig-value\":\"5\"}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(!response.update().atomic());

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
    sr_sess->applyChanges();
}

TEST_CASE("Subscribe (on-change, update with composite key)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data", "baz");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']",
                  sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\",\"@data\":{\"ietf-"
                            "origin:origin\":\"ietf-origin:unknown\"}}"));
    auto resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    sr_sess->deleteItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    REQUIRE(response.update().delete__size() == 1);
    resppath = response.update().delete_().Get(0);
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));
}

TEST_CASE("Subscribe for leaf (on-change, delete and add)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "7");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']/counter", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"7\""));
    auto path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Delete then add - sysrepo generates both delete and create events, check that we handle that
    sr_sess->dropForeignOperationalContent("/gnmi-server-test:test-state/things");
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "8");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    REQUIRE(response.update().delete__size() == 1);
    path = path_to_xpath(response.update().delete_().Get(0));
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("\"8\",\"@counter\":{\"yang:operation\":\"replace\",\"yang:orig-"
                            "default\":false,\"yang:orig-value\":\"7\"}"));
    path = path_to_xpath(response.update().update().Get(0).path());
    CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
    CHECK(!response.update().atomic());

    // Update then delete - sysrepo collapses this to nothing
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "8");
    sr_sess->dropForeignOperationalContent("/gnmi-server-test:test-state/things");
    sr_sess->applyChanges();

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
    sr_sess->applyChanges();
}

TEST_CASE("Subscribe with non-existent path (once)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == true);

    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    success = rw->WritesDone();
    CHECK(success == true);
    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe with non-existent path (poll)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Write(poll_request);
    CHECK(success == true);
    success = rw->WritesDone();
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.ok());
}

TEST_CASE("Subscribe with non-existent path (stream-sample)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    std::chrono::seconds interval(1);

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::SAMPLE);
    sub->set_sample_interval(std::chrono::nanoseconds(interval).count());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Wait for one update after the initial one
    success = rw->Read(&response);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    // And then cancel
    ctx.TryCancel();
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
}

TEST_CASE("Subscribe with non-existent path (on-change)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    std::chrono::seconds interval(1);

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);
    sub->set_sample_interval(std::chrono::nanoseconds(interval).count());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // And then cancel
    ctx.TryCancel();
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
}

TEST_CASE("Subscribe (on-change, 2 subscriptions, delete)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    auto sub2 = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data", "baz");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']",
                  sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foofoo']",
                  sub2->mutable_path());
    sub2->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update for 1st subscription
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\",\"@data\":{\"ietf-"
                            "origin:origin\":\"ietf-origin:unknown\"}}"));
    auto resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(!response.update().atomic());

    // Empty initial update for 2nd subscription (no data)
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);

    // Sync response
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Delete for 1 subscription
    sr_sess->deleteItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    REQUIRE(response.update().delete__size() == 1);
    resppath = response.update().delete_().Get(0);
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    // Update for the other subscription
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foofoo']/data", "baz");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foofoo\",\"data\":\"baz\"}"));
    resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foofoo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(!response.update().atomic());

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));
}

TEST_CASE("Subscribe (on-change, 2 subscriptions different modules, update/delete)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    auto sub2 = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data", "baz");
    sr_sess->setItem(
        "/gnmi-server-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']/score", "98");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']",
                  sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);
    xpath_to_path("/gnmi-server-test-wine:wines", sub2->mutable_path());
    sub2->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial update for 1st subscription
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\",\"@data\":{\"ietf-"
                            "origin:origin\":\"ietf-origin:unknown\"}}"));
    auto resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(!response.update().atomic());

    // Initial update for 2nd subscription
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"wine\":[{\"name\":\"Mas La "
                            "Plana\",\"vintage\":1985,\"score\":98,\"@score\":{\"ietf-origin:"
                            "origin\":\"ietf-origin:unknown\"}}]}"));
    resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 1);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test-wine:wines"));
    CHECK(!response.update().atomic());

    // Sync
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Delete for 1 subscription
    sr_sess->deleteItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    REQUIRE(response.update().delete__size() == 1);
    resppath = response.update().delete_().Get(0);
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 0);
    CHECK(!response.update().atomic());

    // Update for the other subscription
    sr_sess->setItem(
        "/gnmi-server-test-wine:wines/wine[name='Mas La Plana'][vintage='1985']/score", "99");
    sr_sess->applyChanges();

    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    // This is an update (i.e. modify)
    CHECK(response.update().delete__size() == 1);
    CHECK(response.update().update_size() == 1);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("99,\"@score\":{\"yang:operation\":\"replace\",\"yang:orig-default\":"
                            "false,\"yang:orig-value\":\"98\"}"));
    resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 3);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test-wine:wines"));
    CHECK_THAT(resppath.elem(1).name(), Equals("wine"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("Mas La Plana"));
    CHECK_THAT(resppath.elem(1).key().at("vintage"), Equals("1985"));
    CHECK_THAT(resppath.elem(2).name(), Equals("score"));
    CHECK(!response.update().atomic());
    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));
}

TEST_CASE("Subscribe (stream: mix of sample and on-change)", "[subs]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();
    auto sub2 = list->add_subscription();
    std::chrono::seconds interval(1);

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']/data", "baz");
    sr_sess->setItem("/gnmi-server-test:test3/complex-list[type='bar'][name='foofoo']/data", "baz");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foo']",
                  sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::SAMPLE);
    sub->set_sample_interval(std::chrono::nanoseconds(interval).count());
    xpath_to_path("/gnmi-server-test:test3/complex-list[type='bar'][name='foofoo']",
                  sub2->mutable_path());
    sub2->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    // Initial data should contain data for sample subscription
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);

    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    auto json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\"}"));
    auto resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));

    // Next response is initial-data for on-change subscription
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.sync_response());
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);

    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foofoo\",\"data\":\"baz\",\"@data\":{"
                            "\"ietf-origin:origin\":\"ietf-origin:unknown\"}}"));
    resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foofoo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));

    CHECK(!response.update().atomic());

    // Sync response
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(!response.has_update());
    CHECK(response.sync_response());

    // Wait for one update (sample only) after the initial one
    success = rw->Read(&response);
    CHECK(success == true);
    CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
    CHECK(response.extension_size() == 0);
    CHECK(response.update().delete__size() == 0);
    CHECK(response.update().timestamp() > 0);
    CHECK(!response.update().has_prefix());
    CHECK_THAT(response.update().alias(), Equals(""));
    REQUIRE(response.update().update_size() == 1);
    CHECK(response.update().update().Get(0).val().value_case() ==
          gnmi::TypedValue::ValueCase::kJsonIetfVal);
    json = response.update().update().Get(0).val().json_ietf_val();
    CHECK_THAT(json, Equals("{\"type\":\"bar\",\"name\":\"foo\",\"data\":\"baz\"}"));
    resppath = response.update().update().Get(0).path();
    CHECK(resppath.elem_size() == 2);
    CHECK_THAT(resppath.elem(0).name(), Equals("gnmi-server-test:test3"));
    CHECK_THAT(resppath.elem(1).name(), Equals("complex-list"));
    CHECK_THAT(resppath.elem(1).key().at("name"), Equals("foo"));
    CHECK_THAT(resppath.elem(1).key().at("type"), Equals("bar"));

    // And then cancel
    ctx.TryCancel();
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
}

static void update_counter()
{
    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    for (uint32_t i = 1; i <= 20; i++)
    {
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter",
                         std::to_string(i).c_str());
        sr_sess->applyChanges();
    }
}

TEST_CASE("Subscribe for leaf (on-change, race condition)", "[subs-scale]")
{
    ScaleTestLogLevelReducer _log_reducer;
    for (uint32_t i = 0; i < 2000; i++)
    {
        grpc::ClientContext ctx;
        gnmi::SubscribeRequest request;
        gnmi::SubscribeRequest poll_request;
        gnmi::SubscribeResponse response;

        auto list = request.mutable_subscribe();
        auto sub = list->add_subscription();

        sr_sess->switchDatastore(sysrepo::Datastore::Operational);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter",
                         std::to_string(0).c_str());
        sr_sess->applyChanges();

        list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
        list->set_encoding(gnmi::Encoding::JSON_IETF);
        xpath_to_path("/gnmi-server-test:test-state/things[name='C']/counter", sub->mutable_path());
        sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

        // This is to have race condition where the data is being modified
        // around same time it's being subscribed to below.
        auto update_thread = std::thread(update_counter);

        auto rw = client->Subscribe(&ctx);
        auto success = rw->Write(request);
        CHECK(success == true);

        // Initial update
        success = rw->Read(&response);
        CHECK(success == true);
        CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
        CHECK(response.extension_size() == 0);
        CHECK(response.update().delete__size() == 0);
        CHECK(response.update().timestamp() > 0);
        CHECK(!response.update().has_prefix());
        CHECK_THAT(response.update().alias(), Equals(""));
        REQUIRE(response.update().update_size() == 1);
        CHECK(response.update().update().Get(0).val().value_case() ==
              gnmi::TypedValue::ValueCase::kJsonIetfVal);
        auto json = response.update().update().Get(0).val().json_ietf_val();
        // We don't check the value since it can be 0...max
        // CHECK_THAT(json, Equals("\"0\""));
        auto path = path_to_xpath(response.update().update().Get(0).path());
        CHECK_THAT(path, Equals("/gnmi-server-test:test-state/things[name='C']/counter"));
        CHECK(!response.update().atomic());

        success = rw->Read(&response);
        CHECK(success == true);
        CHECK(!(response.response_case() == gnmi::SubscribeResponse::ResponseCase::kError));
        CHECK(response.extension_size() == 0);
        CHECK(!response.has_update());
        CHECK(response.sync_response());

        ctx.TryCancel();

        success = rw->Read(&response);
        CHECK(success == false);

        update_thread.join();

        auto status = rw->Finish();
        CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
        CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));

        // Clean up
        sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
        sr_sess->applyChanges();
    }
}

TEST_CASE("Subscribe for leaf (on-change, slow client)", "[subs-scale]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;

    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    sr_sess->switchDatastore(sysrepo::Datastore::Operational);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']", std::nullopt);
    sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter", "0");
    sr_sess->applyChanges();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state/things[name='C']/counter", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    ScaleTestLogLevelReducer _log_reducer;

    // Just generate a huge amount of updates without reading so that the Write call from the server
    // hangs This shouldn't cause any delay for the applyChanges call, which would normally be a
    // system component.
    for (uint32_t i = 0; i < 25000; i++)
    {
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='C']/counter",
                         std::to_string(i).c_str());
        sr_sess->applyChanges();
    }

    ctx.TryCancel();

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::CANCELLED);
    CHECK_THAT(status.error_message(), Contains("cancelled", Catch::CaseSensitive::No));

    // Clean up
    sr_sess->deleteItem("/gnmi-server-test:test-state/things[name='C']");
    sr_sess->applyChanges();
}

/* Negative tests */

TEST_CASE("Subscribe (empty)", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("SubscribeRequest needs non-empty SubscriptionList"));
}

TEST_CASE("Subscribe (once) with invalid mode", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(static_cast<gnmi::SubscriptionList_Mode>(42));
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:non-existent", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Unknown subscription mode"));
}

TEST_CASE("Subscribe (once) with unsupported encoding type", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::BYTES);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("BYTES"));
}

TEST_CASE("Subscribe (once) with another unsupported encoding type", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_ONCE);
    list->set_encoding(gnmi::Encoding::PROTO);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);
    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("PROTO"));
}

TEST_CASE("Subscribe (poll) with use_aliases", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    list->set_use_aliases(true);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("alias not supported"));
}

TEST_CASE("Subscribe (poll) with updates_only", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    list->set_updates_only(true);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("updates-only not supported"));
}

TEST_CASE("Subscribe (poll) with alias request", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_aliases();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("Aliases not implemented yet"));
}

TEST_CASE("Subscribe (poll) with dup sub request", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_subscribe();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(),
               Equals("A SubscriptionList has already been received for this RPC"));
}

TEST_CASE("Subscribe (stream-sample) with huge sample interval", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::SAMPLE);
    sub->set_sample_interval(std::numeric_limits<uint64_t>::max());

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(),
               Equals("sample_interval must be less than 9223372036854775807 nanoseconds"));
}

TEST_CASE("Subscribe (stream) with use_aliases", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    list->set_use_aliases(true);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    sub->set_mode(gnmi::SubscriptionMode::ON_CHANGE);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("alias not supported"));
}

TEST_CASE("Subscribe (stream) with updates_only", "[subs-neg]")
{
    grpc::ClientContext ctx;
    gnmi::SubscribeRequest request;
    gnmi::SubscribeRequest poll_request;
    gnmi::SubscribeResponse response;
    auto list = request.mutable_subscribe();
    auto sub = list->add_subscription();

    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_STREAM);
    list->set_encoding(gnmi::Encoding::JSON_IETF);
    list->set_updates_only(true);
    xpath_to_path("/gnmi-server-test:test-state", sub->mutable_path());
    list->set_mode(gnmi::SubscriptionList_Mode::SubscriptionList_Mode_POLL);

    auto rw = client->Subscribe(&ctx);
    auto success = rw->Write(request);
    CHECK(success == true);

    poll_request.mutable_poll();
    success = rw->Write(poll_request);
    CHECK(success == true);

    success = rw->Read(&response);
    CHECK(success == false);

    auto status = rw->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(status.error_message(), Equals("updates-only not supported"));
}
