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

#include <chrono>
#include <csignal>
#include <exception>
#include <getopt.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <proto/gnmi.grpc.pb.h>
#include <proto/yang_rpc.grpc.pb.h>

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/utils/exception.hpp>

#include <gnmi/gnmi.h>
#include <security/auth.h>
#include <utils/log.h>
#include <yang_rpc/yang_rpc.h>

static struct
{
    std::unique_ptr<grpc::Server> server;
    int pipefd[2];
} g_state;

extern "C" void signal_handler(int signum)
{
    if (write(g_state.pipefd[1], &signum, sizeof(signum)) < 0)
    {
        exit(2);
    }
}

void SetupSignalHandler(void)
{
    // Set up the signal handler
    if (pipe(g_state.pipefd) < 0)
    {
        std::cerr << "Failed to create signal handler pipe " << strerror(errno) << std::endl;
        exit(1);
    }

    // Block all signals for the main thread and other new threads
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // Register the signal handler
    struct sigaction sa;
    sa.sa_handler = &signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

static void wait_for_terminate(void)
{
    int signal = 0;

    // UnBlock all signals for this thread
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    while (read(g_state.pipefd[0], &signal, sizeof(signal)) < 0)
    {
        // ignore interrupted system call
    }

    SLOG_DEBUG("Shutting down due to ", strsignal(signal), " signal");
    GNMIService::TryCancelAll();
    // deadline set to 100 ms so that we do not have to wait during shutdown
    g_state.server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
}

void RunServer(std::string bind_addr, Auth &auth)
{
    // Get log environment variable
    slog::get_log_env();

    sysrepo::Connection sr_conn = sysrepo::Connection();
    std::shared_ptr<grpc::ServerCredentials> cred = auth.init();
    GNMIService gnmi(sr_conn, auth); // gNMI Service

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(bind_addr, cred, &selected_port);
    builder.RegisterService(&gnmi);
#ifdef GNXI_SERVICE_ENABLED
    YANG_RPCService yang_rpc(sr_conn); // gNXI Service is only compiled in when enabled
    builder.RegisterService(&yang_rpc);
#endif
    g_state.server = builder.BuildAndStart();

    if (g_state.server == nullptr)
    {
        SLOG_ERROR("Failed to build gRPC server");
        exit(1);
    }

    if (selected_port != 0)
    {
        SLOG_INFO("Server listening on ", bind_addr, " (port: ", selected_port, ")");
    }
    else
    {
        SLOG_INFO("Server listening on ", bind_addr);
    }

    wait_for_terminate();

    SLOG_INFO("GNMI Server exited");
}

const char *USAGE = R"(Usage:
  sysrepo-gnxi -f [-l LOG_LEVEL] [-b URI]
  sysrepo-gnxi -k PATH -c PATH -r PATH -u PATH [-l LOG_LEVEL] [-b URI]

Options:
  -h,--help                 Show help
  -f,--force-insecure       Insecure connection (no TLS, no passwords)
  -k,--private-key PATH     Path to server TLS private key
  -c,--cert PATH            Path to server TLS certificate
  -r,--ca PATH              Path to root certificate/CA certificate
  -u,--userdb PATH          Path to user database JSON file
  -l,--log-level LOG_LEVEL  Logging level
    0 = log fatal messages
    1 = log error messages and all above
    2 = (default) log warning messages and all above
    3 = log informational messages and all above
    4 = log debug messages and all above
  -b,--bind URI
    [PREFIX] HOST [":" PORT]
    unix:/path/to/socket

    defaults:
      PREFIX = dns:///
      HOST = 127.0.0.1
      PORT = 443
)";

int main(int argc, char *argv[])
{
    int c, option_index = 0;
    extern char *optarg;
    std::string bind_addr = "127.0.0.1";
    Auth auth;

    static struct option long_options[] = {{"help", no_argument, 0, 'h'},
                                           {"force-insecure", no_argument, 0, 'f'},
                                           {"private-key", required_argument, 0, 'k'},
                                           {"cert", required_argument, 0, 'c'},
                                           {"ca", required_argument, 0, 'r'},
                                           {"userdb", required_argument, 0, 'u'},
                                           {"log-level", required_argument, 0, 'l'},
                                           {"bind", required_argument, 0, 'b'},
                                           {0, 0, 0, 0}};

    while ((c = getopt_long(argc, argv, "hfk:c:r:u:l:b:", long_options, &option_index)) != -1)
    {
        switch (c)
        {
        case 'f': // force insecure connection
            auth.insecure = true;
            break;
        case 'k': // server private key
            auth.private_key_path = std::string(optarg);
            break;
        case 'c': // server certificate
            auth.cert_path = std::string(optarg);
            break;
        case 'r': // CA/root certificate
            auth.root_cert_path = std::string(optarg);
            break;
        case 'u': // user database
            auth.user_db_path = std::string(optarg);
            break;
        case 'l': // log level
            slog::set_level(std::atoi(optarg));
            break;
        case 'b': // binding address
            bind_addr = std::string(optarg);
            break;
        case '?': // help
        case 'h':
            std::cout << USAGE;
            exit(0);
        default: // unhandled option
            std::cerr << USAGE;
            exit(1);
        }
    }

    SetupSignalHandler();

    try
    {
        RunServer(bind_addr, auth);
    }
    catch (const std::exception &exc)
    {
        SLOG_FATAL("GNMI server aborted: ", exc.what());
        exit(1);
    }

    return 0;
}
