/**
 * @file rpc.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Rpc RPC tests
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

#include "catch2/catch.hpp"
#include <memory>

#include <grpcpp/grpcpp.h>

#include <sysrepo-cpp/Connection.hpp>

#include "main.h"

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

// positive tests

TEST_CASE("Rpc rpc", "[rpc]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"eth45\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.ok());
    CHECK(response.timestamp() > 0);
    CHECK_THAT(response.val().json_ietf_val(), Equals("{\"old-stats\":\"613\"}"));
}

TEST_CASE("Rpc action", "[rpc]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:action-test/action-test", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"foo\": \"bar\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.ok());
    CHECK(response.timestamp() > 0);
    CHECK_THAT(response.val().json_ietf_val(), Equals("{\"bar\":\"action-result\"}"));
}

// negative tests

TEST_CASE("Rpc (no path)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    request.mutable_path();
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"eth45\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (no value)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val();
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (unsupported encoding)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"eth45\"}");
    request.set_encoding(gnmi::BYTES);
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (unsupported input type)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_string_val("eth45");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (nonexistent path)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:nonexistent", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"eth45\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (malformed value)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{bad");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (callback error)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"error\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK_THAT(status.error_message(), Contains("Fiddlesticks: /gnmi-server-test:clear-stats"));
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (no subscriber)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:no-subscriber", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"foo\": \"bar\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}

TEST_CASE("Rpc (timeout)", "[rpc-neg]")
{
    grpc::ClientContext ctx;
    gnxi::RpcRequest request;
    gnxi::RpcResponse response;

    xpath_to_path("/gnmi-server-test:clear-stats", request.mutable_path());
    request.mutable_val()->set_json_ietf_val("{\"interface\": \"timeout\"}");
    auto status = gnxi_client->Rpc(&ctx, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED);
    CHECK(response.timestamp() == 0);
    CHECK(response.val().value_case() == gnmi::TypedValue::VALUE_NOT_SET);
}
