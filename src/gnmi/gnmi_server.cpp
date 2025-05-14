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

#include "gnmi/gnmi.h"
#include <security/authentication.h>
#include <utils/log.h>

using namespace std;

static struct {
  unique_ptr<Server> server;
  int pipefd[2];
} g_state;

extern "C" void signal_handler(int signum) {
  if (write(g_state.pipefd[1], &signum, sizeof signum) < 0) {
    exit(2);
  }
}

void SetupSignalHandler(bool is_daemon)
{
  // Set up the signal handler
  if (pipe(g_state.pipefd) < 0) {
    cerr << "Failed to create signal handler pipe " << strerror(errno) << endl;
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
  if (is_daemon) {
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

  while (read(g_state.pipefd[0], &signal, sizeof signal) < 0) {
    // ignore interrupted system call
  }

  BOOST_LOG_TRIVIAL(debug) << "Shutting down due to " << strsignal(signal) << " signal";
  GNMIService::TryCancelAll();

  g_state.server->Shutdown();
}

void RunServer(string bind_addr, shared_ptr<ServerCredentials> cred, sysrepo::Connection sr_conn, std::promise<void> ready)
{
  // Get log environment variable
  get_log_env();

  try {
    GNMIService gnmi(sr_conn); //gNMI Service

    ServerBuilder builder;
    builder.AddListeningPort(bind_addr, cred);
    builder.RegisterService(&gnmi);
    g_state.server = builder.BuildAndStart();
    ready.set_value();

    if (g_state.server == nullptr) {
      BOOST_LOG_TRIVIAL(error) << "Failed to build gRPC server";
      exit(1);
    }

    if (bind_addr.find(":") == string::npos) {
      BOOST_LOG_TRIVIAL(info) << "Server listening on " << bind_addr << ":443";
    } else {
      BOOST_LOG_TRIVIAL(info) << "Server listening on " << bind_addr;
    }

    wait_for_terminate();
  } catch (sysrepo::ErrorWithCode &exc) {
    BOOST_LOG_TRIVIAL(error) << "Connection to sysrepo failed " << exc.what();
    exit(1);
  }

  BOOST_LOG_TRIVIAL(info) << "GNMI Server exited";
}

