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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sysrepo-cpp/Session.hpp>
#include <vector>

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

/**
 * @brief Parse an XPath-like string into a gNMI Path.
 *
 * @param[in] xpath XPath-like string to parse.
 * @param[out] path gNMI path to fill from the parsed elements.
 * @param[in] origin Origin to set on the path (empty for no origin).
 */
extern void xpath_to_path(std::string xpath, gnmi::Path *path, std::string origin = "rfc7951");

/**
 * @brief Render a gNMI Path as an XPath-like string, keys sorted for determinism.
 *
 * @param[in] path gNMI path to render.
 * @return XPath-like string representation of the path.
 */
extern std::string path_to_xpath(const gnmi::Path &path);

/**
 * @brief Create an insecure gRPC channel to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
extern std::shared_ptr<grpc::Channel> make_insecure_channel(const std::string &addr);

/**
 * @brief Create a TLS gRPC channel without a client certificate to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
extern std::shared_ptr<grpc::Channel> make_tls_channel(const std::string &addr);

/**
 * @brief Create an mTLS gRPC channel with the test client certificate to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
extern std::shared_ptr<grpc::Channel> make_mtls_channel(const std::string &addr);

/**
 * @brief Create an mTLS gRPC channel with the second test client certificate to the given address.
 *
 * @param[in] addr Address to connect to.
 * @return New gRPC channel.
 */
extern std::shared_ptr<grpc::Channel> make_mtls_channel2(const std::string &addr);

/**
 * @brief Prepare a client context with optional credentials metadata.
 *
 * @param[in] ctx Client context to prepare.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 */
extern void prepare_context(
    grpc::ClientContext &ctx, const std::string &username = "", const std::string &password = "");

/**
 * @brief Call the Capabilities RPC on a channel with optional credentials.
 *
 * @param[in] channel Channel to use.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return RPC status.
 */
extern grpc::Status do_capabilities(const std::shared_ptr<grpc::Channel> &channel,
    const std::string &username = "", const std::string &password = "");

/**
 * @brief Check that a Capabilities RPC succeeds over a fresh channel.
 *
 * @param[in] make_channel Channel factory.
 * @param[in] addr Address to connect to.
 * @param[in] username Username metadata to add (empty for none).
 * @param[in] password Password metadata to add (empty for none).
 * @return True when the RPC succeeded within the timeout.
 */
extern bool eventually_reachable(
    std::shared_ptr<grpc::Channel> (*make_channel)(const std::string &), const std::string &addr,
    const std::string &username = "", const std::string &password = "");

/**
 * @brief Apply a complete JSON configuration to the running datastore.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] file JSON configuration file to apply.
 * @param[in] sock_path Socket path to substitute for the placeholder.
 */
extern void apply_config(
    sysrepo::Session &sess, const std::filesystem::path &file, const std::string &sock_path);

/**
 * @brief Manages the server subprocess of a test binary. Exactly one
 * instance is expected (the global "server").
 *
 */
class SetupServer
{
  public:
    ~SetupServer();

    /**
     * @brief Launch the server subprocess for this test binary. Must be
     * called (once) before any other method.
     *
     * @param[in] test_name Name of the test (for per-binary paths).
     * @param[in] debug Wait for the user before starting the server.
     * @param[in] secure Launch the secure (mTLS) server instead of the
     * insecure one.
     */
    void launch(const std::string &test_name, bool debug = false, bool secure = false);

    /**
     * @brief Wait for the launched server to become ready.
     *
     * @throws std::runtime_error if the server does not start listening
     * within the timeout.
     */
    void wait_ready();

    /**
     * @brief Stop the launched server subprocess and start a new one on
     * a fresh socket, renewing the gNMI client handles. Used by tests
     * that exercise server shutdown behavior. Only the insecure server
     * is supported.
     *
     * @throws std::runtime_error if the server is not running or is the
     * secure one.
     */
    void restart();

  private:
    /**
     * @brief Stop process by killing it and waiting for it.
     *
     * @param[in] pid Id of a process to kill.
     */
    static void stop_(pid_t pid);

    /**
     * @brief Create a separate process and run server on it.
     *
     * @param[in] args Arguments for the server.
     * @param[in] log_path Log path for server's stdout/stderr.
     * @return New process id.
     */
    pid_t run_server_(const std::vector<std::string> &args, const std::string &log_path);

    /**
     * @brief Wait for server to start listening on the unix socket.
     *
     * @param[in] sock_path Socket to check.
     */
    static void wait_for_unix_(const std::string &sock_path);

    /**
     * @brief Wait for server to start listening on specific port on host.
     *
     * @param[in] host Host address.
     * @param[in] port Host port.
     */
    static void wait_for_tcp_(const std::string &host, uint16_t port);

    static constexpr const char *HOST = "127.0.0.1";
    static constexpr uint16_t PORT = 50052;

    pid_t server_pid_ = -1;
    std::vector<std::string> server_args_;
    std::string server_log_;
    std::string server_label_;
    std::filesystem::path sock_base_;
    std::filesystem::path sock_;
    bool secure_ = false;
    bool debug_ = false;
    int restart_count_ = 0;
};

// the single server subprocess manager of this test binary
extern SetupServer server;
