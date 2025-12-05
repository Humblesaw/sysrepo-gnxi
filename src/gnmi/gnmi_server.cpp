/*
 * Copyright 2020 Yohan Pipereau
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

#include <csignal>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/utils/exception.hpp>

#include "gnmi/gnmi.h"
#include "security/authentication.h"
#include "utils/log.h"

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

void SetupSignalHandler(bool is_daemon)
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

    // tests can receive SIGINT from user to interrupt the tests under gdb.
    // So, only register for SIGINT, if we are running in daemon mode
    if (is_daemon)
    {
        sigaction(SIGINT, &sa, NULL);
    }
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

    g_state.server->Shutdown();
}

void RunServer(std::string bind_addr, std::shared_ptr<grpc::ServerCredentials> cred,
               sysrepo::Connection sr_conn, std::promise<void> ready)
{
    // Get log environment variable
    slog::get_log_env();

    try
    {
        GNMIService gnmi(sr_conn); // gNMI Service

        grpc::ServerBuilder builder;
        builder.AddListeningPort(bind_addr, cred);
        builder.RegisterService(&gnmi);
        g_state.server = builder.BuildAndStart();
        ready.set_value();

        if (g_state.server == nullptr)
        {
            SLOG_ERROR("Failed to build gRPC server");
            exit(1);
        }

        if (bind_addr.find(":") == std::string::npos)
        {
            SLOG_INFO("Server listening on ", bind_addr, ":443");
        }
        else
        {
            SLOG_INFO("Server listening on ", bind_addr);
        }

        wait_for_terminate();
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR("Connection to sysrepo failed ", exc.what());
        exit(1);
    }

    SLOG_INFO("GNMI Server exited");
}
