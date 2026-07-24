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

#include "test_main.h"
#include <grpcpp/grpcpp.h>
#include <sysrepo-cpp/Connection.hpp>

using Catch::Matchers::Equals;

TEST_CASE("Capability request", "[caps]")
{
    grpc::ClientContext ctx;
    gnmi::CapabilityRequest request;
    gnmi::CapabilityResponse response;
    bool found = false;

    auto status = client->Capabilities(&ctx, request, &response);
    CHECK(status.ok());

    REQUIRE(response.supported_encodings().size() == 1);
    CHECK(response.supported_encodings().Get(0) == gnmi::Encoding::JSON_IETF);
    for (auto m : response.supported_models())
    {
        if (!m.name().compare("gnmi-server-test"))
        {
            CHECK_THAT(m.version(), Equals("2021-02-10"));
            CHECK_THAT(m.organization(), Equals(""));
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(!response.gnmi_version().compare("0.10.0"));
}
