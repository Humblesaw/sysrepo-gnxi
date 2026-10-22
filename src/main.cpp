/**
 * @file main.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Main executable implementation
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
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

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <libyang-cpp/DataNode.hpp>

#include <proto/gnmi.grpc.pb.h>
#include <proto/yang_rpc.grpc.pb.h>

#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/utils/exception.hpp>

#include <gnmi/gnmi.h>
#include <security/auth.h>
#include <security/crypto.h>
#include <utils/log.h>
#include <yang_rpc/yang_rpc.h>

/**
 * @brief Global state shared between the server and the signal handler.
 *
 */
static struct
{
    std::unique_ptr<grpc::Server> server;
    int pipefd[2];

    // NACM subscriptions, alive while the server runs
    sr_subscription_ctx_t *nacm_sub = nullptr;
    sr_subscription_ctx_t *nacm_stats_sub = nullptr;
    std::optional<sysrepo::Session> nacm_stats_sess;
} g_state;

/**
 * @brief Forward a received signal number to the main loop through the self-pipe.
 *
 * @param[in] signum Number of the received signal.
 */
extern "C" void signal_handler(int signum)
{
    if (write(g_state.pipefd[1], &signum, sizeof(signum)) < 0)
    {
        exit(2);
    }
}

/**
 * @brief Set up the signal handling for a graceful shutdown.
 *
 */
void setup_signal_handler(void)
{
    // set up the signal handler
    if (pipe(g_state.pipefd) < 0)
    {
        SLOG_FATAL("Failed to create signal handler pipe: ", strerror(errno));
        exit(1);
    }

    // block all signals for the main thread and other new threads
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // register the signal handler
    struct sigaction sa;
    sa.sa_handler = &signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

/**
 * @brief Wait for a termination signal and shutdown the server.
 *
 */
static void wait_for_terminate(void)
{
    int signal = 0;

    // unblock all signals for this thread
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    while (read(g_state.pipefd[0], &signal, sizeof(signal)) < 0)
    {
        // ignore interrupted system call
    }

    SLOG_DEBUG("Shutting down due to ", strsignal(signal), " signal");
    GNMIService::try_cancel_all();
    // deadline set to 100 ms so that we do not have to wait during shutdown
    g_state.server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
}

/**
 * @brief Read a single configuration node by its absolute path. Throws on error.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] path Absolute path to the leaf.
 * @param[in] what Description of the leaf.
 * @return Leaf value.
 */
static std::string config_value(
    sysrepo::Session &sess, const std::string &path, const std::string &what)
{
    auto data = sess.getData(path);
    if (!data.has_value())
    {
        throw std::runtime_error(what + " not found");
    }

    auto node = data->findPath(path);
    if (!node.has_value())
    {
        throw std::runtime_error(what + " not found");
    }
    return node->asTerm().valueStr();
}

/**
 * @brief Read all configuration nodes matching an XPath.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] xpath XPath to find.
 * @return Matching nodes.
 */
static std::vector<libyang::DataNode> config_values(
    sysrepo::Session &sess, const std::string &xpath)
{
    auto data = sess.getData(xpath);
    if (!data.has_value())
    {
        return {};
    }

    auto nodes = data->findXPath(xpath);
    std::vector<libyang::DataNode> values;
    for (auto node : nodes)
    {
        values.push_back(node);
    }
    return values;
}

/**
 * @brief Read the configured listen endpoints and build gRPC listen URIs.
 *
 * @param[in] sess Sysrepo session.
 * @return gRPC listen URIs, e.g. 127.0.0.1:50051 or unix:/path.
 */
static std::vector<std::string> load_bind_addrs(sysrepo::Session &sess)
{
    std::vector<std::string> addrs;

    auto data = sess.getData("/sysrepo-gnxi-server:server/listen/endpoint");
    if (!data.has_value())
    {
        return addrs;
    }

    for (auto endpoint : data->findXPath("/sysrepo-gnxi-server:server/listen/endpoint"))
    {
        auto name = endpoint.findPath("name")->asTerm().valueStr();

        // each endpoint selects exactly one transport (mandatory choice)
        if (auto path = endpoint.findPath("unix/path"))
        {
            addrs.push_back("unix:" + path->asTerm().valueStr());
            continue;
        }

        if (auto address = endpoint.findPath("dns/address"))
        {
            auto port = endpoint.findPath("dns/port");
            addrs.push_back("dns:///" + address->asTerm().valueStr() + ":" +
                            (port ? port->asTerm().valueStr() : std::string("50051")));
            continue;
        }

        if (auto address = endpoint.findPath("ip/address"))
        {
            auto port = endpoint.findPath("ip/port");
            auto addr = address->asTerm().valueStr();
            auto port_str = port ? port->asTerm().valueStr() : std::string("50051");
            if (addr.find(':') != std::string::npos)
            {
                // IPv6 addresses must be enclosed in brackets
                addrs.push_back("[" + addr + "]:" + port_str);
            }
            else
            {
                addrs.push_back(addr + ":" + port_str);
            }
            continue;
        }

        throw std::runtime_error("listen endpoint '" + name + "' has no transport configured");
    }

    return addrs;
}

/**
 * @brief Load the TLS server material from sysrepo.
 *
 * Reads the server identity (private key and certificate chain from
 * ietf-keystore) and the client verification CA bundle (ietf-truststore)
 * referenced by the server configuration. The stored base64 bodies are
 * wrapped into PEM for gRPC.
 *
 * @param[in] sess Sysrepo session.
 * @return TLS material wrapped into PEM.
 */
static Auth::TlsMaterial load_tls_config(sysrepo::Session &sess)
{
    Auth::TlsMaterial tls;

    // the referenced key/certificate/bag names fluctuate, read them first
    std::string key_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/asymmetric-key",
        "TLS server identity asymmetric-key reference");
    std::string cert_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/certificate",
        "TLS server identity certificate reference");
    std::string bag_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/client-authentication/ca-certs/"
        "central-truststore-reference",
        "TLS client authentication CA-certs truststore reference");

    // private key of the server identity
    std::string key_path =
        "/ietf-keystore:keystore/asymmetric-keys/asymmetric-key[name='" + key_name + "']";
    tls.private_key_pem =
        pem_wrap_private_key(config_value(sess, key_path + "/cleartext-private-key",
                                 "cleartext-private-key of asymmetric-key '" + key_name + "'"),
            config_value(sess, key_path + "/private-key-format",
                "private-key-format of asymmetric-key '" + key_name + "'"));

    // server certificate chain - the referenced certificate first, then the
    // remaining certificates of the same asymmetric key (intermediates)
    tls.certificate_pem = pem_wrap_certificate(config_value(sess,
        key_path + "/certificates/certificate[name='" + cert_name + "']/cert-data",
        "certificate '" + cert_name + "' of asymmetric-key '" + key_name + "'"));
    for (auto node : config_values(
             sess, key_path + "/certificates/certificate[not(name='" + cert_name + "')]/cert-data"))
    {
        tls.certificate_pem += "\n" + pem_wrap_certificate(node.asTerm().valueStr());
    }

    // CA certificates for client verification
    for (auto node :
        config_values(sess, "/ietf-truststore:truststore/certificate-bags/certificate-bag[name='" +
                                bag_name + "']/certificate/cert-data"))
    {
        if (!tls.ca_certificate_pem.empty())
        {
            tls.ca_certificate_pem += "\n";
        }
        tls.ca_certificate_pem += pem_wrap_certificate(node.asTerm().valueStr());
    }
    if (tls.ca_certificate_pem.empty())
    {
        throw std::runtime_error(
            "certificate-bag '" + bag_name + "' not found or contains no certificates");
    }

    SLOG_INFO("TLS configuration loaded from sysrepo "
              "(key: ",
        key_name, ", cert: ", cert_name, ", CA bag: ", bag_name, ")");

    return tls;
}

/**
 * @brief Check netconf-acm is implemented, initialize NACM and its global
 * stats provider (before any user session is created) and subscribe the global
 * NACM stats provider.
 *
 * @param[in] sess Sysrepo session to initialize NACM over, must stay alive
 * until stop_nacm().
 * @param[in] conn Sysrepo connection for the stats provider session.
 */
static void start_nacm(sysrepo::Session &sess, sysrepo::Connection &conn)
{
    if (!sess.getContext().getModuleImplemented("ietf-netconf-acm"))
    {
        throw std::runtime_error("ietf-netconf-acm module is not implemented in sysrepo, "
                                 "authorization (NACM) not available");
    }

    if (sr_nacm_init(sysrepo::getRawSession(sess), 0, &g_state.nacm_sub) != SR_ERR_OK)
    {
        throw std::runtime_error("NACM initialization failed (sr_nacm_init)");
    }

    // optional global NACM stats, succeed with no-op when another consumer
    // (e.g. netopeer2) already provides them
    g_state.nacm_stats_sess = conn.sessionStart(sysrepo::Datastore::Running);
    if (sr_nacm_glob_stats_subscribe(sysrepo::getRawSession(*g_state.nacm_stats_sess), 0,
            &g_state.nacm_stats_sub) != SR_ERR_OK)
    {
        throw std::runtime_error("NACM global stats subscribe failed");
    }

    SLOG_INFO("NACM initialized (ietf-netconf-acm), per-RPC sessions will be per-user checked");
}

/**
 * @brief Undo start_nacm(): unsubscribe and destroy NACM.
 *
 */
static void stop_nacm()
{
    if (g_state.nacm_stats_sub)
    {
        sr_unsubscribe(g_state.nacm_stats_sub);
    }
    if (g_state.nacm_sub)
    {
        sr_unsubscribe(g_state.nacm_sub);
    }
    sr_nacm_destroy();
}

/**
 * @brief Start the gRPC server and wait for its termination.
 *
 * @param[in] sr_conn Sysrepo connection.
 * @param[in] bind_addrs gRPC listen URIs to bind.
 * @param[in] auth Authentication providing the server credentials.
 */
void run_server(sysrepo::Connection &sr_conn, const std::vector<std::string> &bind_addrs,
    std::shared_ptr<Auth> auth)
{
    std::shared_ptr<grpc::ServerCredentials> cred = auth->credentials();
    GNMIService gnmi(sr_conn, auth);        // gNMI Service
    YangRpcService yang_rpc(sr_conn, auth); // YANG Rpc Service

    grpc::ServerBuilder builder;
    for (const auto &bind_addr : bind_addrs)
    {
        builder.AddListeningPort(bind_addr, cred);
    }
    builder.RegisterService(&gnmi);
    builder.RegisterService(&yang_rpc);
    g_state.server = builder.BuildAndStart();

    if (g_state.server == nullptr)
    {
        SLOG_FATAL("Failed to build gRPC server");
        exit(1);
    }

    for (const auto &bind_addr : bind_addrs)
    {
        SLOG_INFO("Server listening on ", bind_addr);
    }

    wait_for_terminate();

    SLOG_INFO("Server exited");
}

constexpr const char *USAGE = R"(Usage:
  sysrepo-gnxi [-l LOG_LEVEL]
  sysrepo-gnxi -f [-l LOG_LEVEL]

Options:
  -h,--help                 Show help
  -f,--force-insecure       Insecure connection (no TLS, no passwords)
  -l,--log-level LOG_LEVEL  Logging level
    0 = log fatal messages
    1 = log error messages and all above
    2 = log warning messages and all above
    3 = (default) log informational messages and all above
    4 = log debug messages and all above

Server configuration is read from sysrepo.
)";

int main(int argc, char *argv[])
{
    int c, option_index = 0;
    extern char *optarg;
    bool insecure = false;

    static struct option long_options[] = {{"help", no_argument, 0, 'h'},
        {"force-insecure", no_argument, 0, 'f'}, {"log-level", required_argument, 0, 'l'},
        {0, 0, 0, 0}};

    while ((c = getopt_long(argc, argv, "hfl:", long_options, &option_index)) != -1)
    {
        switch (c)
        {
        case 'f': // force insecure connection
            insecure = true;
            break;
        case 'l': // log level
        {
            int level = 0;
            const auto [ptr, ec] = std::from_chars(optarg, optarg + std::strlen(optarg), level);
            if (ec != std::errc() || *ptr != '\0' || level < 0 || level > 4)
            {
                std::cerr << "Invalid log level '" << optarg << "', expected 0-4\n" << USAGE;
                exit(1);
            }
            slog::set_level(level);
            break;
        }
        case '?': // help
        case 'h':
            std::cout << USAGE;
            exit(0);
        default: // unhandled option
            std::cerr << USAGE;
            exit(1);
        }
    }

    setup_signal_handler();

    try
    {
        sysrepo::Connection sr_conn;

        // the config session also hosts the NACM subscriptions created by
        // start_nacm(), so it must stay alive for the whole run of the server
        auto config_sess =
            std::make_optional<sysrepo::Session>(sr_conn.sessionStart(sysrepo::Datastore::Running));
        std::vector<std::string> bind_addrs;
        Auth::TlsMaterial tls;

        // read server configuration
        bind_addrs = load_bind_addrs(*config_sess);
        if (bind_addrs.empty())
        {
            throw std::runtime_error("no listen endpoint configured "
                                     "(sysrepo-gnxi-server:server/listen/endpoint)");
        }
        if (!insecure)
        {
            tls = load_tls_config(*config_sess);

            // initialize NACM before any user session is created
            start_nacm(*config_sess, sr_conn);
        }

        auto auth = std::make_shared<Auth>(insecure, sr_conn, tls);

        run_server(sr_conn, bind_addrs, auth);

        stop_nacm();
    }
    catch (const std::exception &exc)
    {
        SLOG_FATAL("Server aborted: ", exc.what());
        stop_nacm();
        exit(1);
    }

    return 0;
}
