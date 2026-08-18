/**
 * @file test_main.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Main test header
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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <sysrepo-cpp/Session.hpp>

#include "proto/gnmi.grpc.pb.h"
#include "proto/yang_rpc.grpc.pb.h"

// insecure server: unix socket under the build dir
extern std::string insecure_addr;
// mTLS server: 127.0.0.1:50052 (test_auth only, no parallel conflict)
extern std::string mtls_addr;
// gNMI service handle
extern std::unique_ptr<gnmi::gNMI::Stub> gnmi_client;
// gNXI service handle
extern std::unique_ptr<yang_rpc::YANG_RPC::Stub> gnxi_client;
// sysrepo session to inspect data
extern std::optional<sysrepo::Session> sr_sess;

extern void xpath_to_path(std::string xpath, gnmi::Path *path);
extern std::string path_to_xpath(const gnmi::Path &path);
