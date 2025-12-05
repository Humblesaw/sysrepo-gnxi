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

#include <sysrepo-cpp/Connection.hpp>

#include "main.h"

using Catch::Matchers::Equals;

static inline bool ends_with(std::string const &value, std::string const &ending)
{
    if (ending.size() > value.size())
        return false;
    return equal(ending.rbegin(), ending.rend(), value.rbegin());
}

/* Positive Tests */

TEST_CASE("Rpc", "[rpc]")
{
    grpc::ClientContext ctx;
    gnmi::RpcRequest request;
    gnmi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"eth45\"}");
    auto status = client->Rpc(&ctx, request, &response);
    std::cout << status.error_message();
    CHECK(status.ok());

    CHECK(response.timestamp() > 0);
    CHECK_THAT(response.val().json_ietf_val(), Equals("{\"old-stats\":\"613\"}"));
}

/* Negative Tests */

TEST_CASE("Rpc request (empty error)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnmi::RpcRequest request;
    gnmi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"none\", \"errors\": [null]}");
    auto status = client->Rpc(&ctx, request, &response);
    std::cout << status.error_message();
    CHECK(status.ok());

    CHECK(response.timestamp() > 0);
    CHECK_THAT(response.val().json_ietf_val(), Equals("{\"old-stats\":\"613\"}"));
}

TEST_CASE("Rpc request (no val type)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnmi::RpcRequest request;
    gnmi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val();
    auto status = client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(status.error_message(), Equals("Value not set"));

    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc request (error)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnmi::RpcRequest request;
    gnmi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"error\"}");
    auto status = client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK_THAT(status.error_message(), Equals("Fiddlesticks: /gnmi-server-test:clear-stats"));

    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc request (timeout)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnmi::RpcRequest request;
    gnmi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"timeout\"}");
    auto status = client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK(ends_with(status.error_message(), "processing timed out."));

    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}
