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
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <grpcpp/security/server_credentials.h>

#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/utils/exception.hpp>

#include <security/auth.h>
#include <security/credentials.h>
#include <server_manager.h>
#include <utils/log.h>

/**
 * @brief Global state shared between the server and the signal handler.
 *
 */
static struct
{
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
 * @brief Wait for a termination signal and shut the servers down.
 *
 * @param[in] manager Server manager to shut down.
 */
static void wait_for_terminate(ServerManager &manager)
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
    manager.shutdown();
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

        std::shared_ptr<grpc::ServerCredentials> creds;
        std::optional<ReloadableCredentials> reloadable;
        if (insecure)
        {
            creds = grpc::InsecureServerCredentials();
        }
        else
        {
            // loads and validates the TLS material, throws when incomplete
            reloadable.emplace(sr_conn);
            creds = reloadable->credentials();

            // initialize NACM before any user session is created
            start_nacm(*config_sess, sr_conn);
        }

        auto auth = std::make_shared<Auth>(insecure, sr_conn, creds);

        // declared after reloadable so that the credentials outlive the
        // servers using them
        ServerManager manager(sr_conn, auth, creds);
        creds.reset();

        manager.start();

        wait_for_terminate(manager);

        SLOG_INFO("Server exited");
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
