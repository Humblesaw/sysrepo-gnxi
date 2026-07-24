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

#include <csignal>
#include <filesystem>
#include <sysrepo-cpp/Enum.hpp>
#include <thread>

#define CATCH_CONFIG_RUNNER
#include "catch2/catch.hpp"

#include <sysrepo-cpp/Subscription.hpp>

#include "gnmi/gnmi.h"
#include "security/authentication.h"
#include "test_main.h"
#include "utils/log.h"

std::unique_ptr<gnmi::gNMI::Stub> client;
std::unique_ptr<gnxi::gNXI::Stub> gnxi_client;
std::optional<sysrepo::Session> sr_sess;

// Parse XPath-like string in gnmi::Path
void xpath_to_path(std::string xpath, gnmi::Path *path)
{
    path->set_origin("rfc7951");

    if (!xpath.compare("/"))
        return;

    auto start = 0u;
    auto end = xpath.find_first_of('/', start);
    assert(end != std::string::npos);
    // Skip initial / - we don't want an empty path elem inserted for it
    start = end + 1;
    end = xpath.find_first_of('/', start);
    for (; true; start = end + 1, end = xpath.find_first_of('/', start))
    {
        auto elem = path->add_elem();
        auto key_start = xpath.find_first_of('[', start);
        // Parse list key if present
        if (key_start != std::string::npos && key_start < end)
        {
            auto value_end = 0u;
            elem->mutable_name()->assign(xpath.substr(start, key_start - start));

            while (key_start != std::string::npos && key_start < end)
            {
                auto key_end = xpath.find_first_of('=', key_start);
                // May be single or double quote character
                auto quote = xpath[key_end + 1];
                value_end = xpath.find_first_of(quote, key_end + 2);
                // +1 to skip over leading '['
                auto key = xpath.substr(key_start + 1, key_end - key_start - 1);
                // +2 to start to skip over = and '
                auto value = xpath.substr(key_end + 2, value_end - key_end - 2);

                (*elem->mutable_key())[key] = value;

                // find if there is a second key following
                key_start = xpath.find_first_of('[', value_end);

                // if 'end' fell on a slash '/' inside a key, move it on to next
                if (key_start != std::string::npos)
                    end = xpath.find_first_of(']', key_start + 1);
            }
            // skip over the key & value
            start = value_end + 2;
            end = xpath.find_first_of('/', start);
        }
        else
            elem->mutable_name()->assign(xpath.substr(start, end - start));
        if (end == std::string::npos)
            break;
    }
}

std::string path_to_xpath(const gnmi::Path &path)
{
    std::string str = "";

    if (path.elem_size() <= 0)
        return "/";

    if (path.origin().compare("rfc7951"))
        return "bad-origin:" + path.origin();

    // iterate over the list of PathElem of a gNMI path
    for (auto &node : path.elem())
    {
        str += "/" + node.name();
        for (auto key : node.key())
            str += "[" + key.first + "='" + key.second + "']";
    }

    return str;
}

static sysrepo::ErrorCode module_change_cb(sysrepo::Session session, uint32_t sub_id,
                                           std::string_view module_name,
                                           std::optional<std::string_view> xpath,
                                           sysrepo::Event event, uint32_t request_id)
{
    (void)session;
    (void)module_name;
    (void)request_id;
    (void)sub_id;

    if (event == sysrepo::Event::Change && xpath == "/gnmi-server-test:test2/custom-error")
    {
        session.setErrorMessage(std::string("Fiddlesticks: ") + std::string(xpath.value()));
        return sysrepo::ErrorCode::CallbackFailed;
    }

    // Don't do anything with the configuration
    return sysrepo::ErrorCode::Ok;
}

static sysrepo::ErrorCode clear_stats_rpc_cb(sysrepo::Session session, uint32_t sub_id,
                                             std::string_view xpath, const libyang::DataNode input,
                                             sysrepo::Event event, uint32_t request_id,
                                             libyang::DataNode output)
{
    (void)session;
    (void)event;
    (void)request_id;
    (void)sub_id;

    if (input.child() && input.child()->isTerm() && input.child()->asTerm().valueStr() == "error")
    {
        session.setErrorMessage(std::string("Fiddlesticks: ") + std::string(xpath));
        return sysrepo::ErrorCode::CallbackFailed;
    }
    if (input.child() && input.child()->isTerm() && input.child()->asTerm().valueStr() == "timeout")
    {
        // Sleep for a time longer than SR_RPC_CB_TIMEOUT
        sleep(3);
    }

    output.newPath("old-stats", "613", libyang::CreationOptions::Output);

    return sysrepo::ErrorCode::Ok;
}

static sysrepo::ErrorCode action_test_cb(sysrepo::Session session, uint32_t sub_id,
                                         std::string_view xpath, const libyang::DataNode input,
                                         sysrepo::Event event, uint32_t request_id,
                                         libyang::DataNode output)
{
    (void)session;
    (void)sub_id;
    (void)xpath;
    (void)input;
    (void)event;
    (void)request_id;

    output.newPath("bar", "action-result", libyang::CreationOptions::Output);

    return sysrepo::ErrorCode::Ok;
}

class SetupSysrepo
{
  public:
    SetupSysrepo()
    {
        auto sr_conn = sysrepo::Connection();
        std::filesystem::path dir{TESTS_SRC_DIR};
        std::filesystem::path files_dir = dir / "files";
        std::filesystem::path path1 = files_dir / "gnmi-server-test.yang";
        std::filesystem::path path2 = files_dir / "gnmi-server-test-wine.yang";
        std::vector<struct sysrepo::ModuleInstallation> modules = {{.schema = path1},
                                                                   {.schema = path2}};

        // install yang modules
        sr_conn.installModules(modules, {files_dir});

        sr_sess = sr_conn.sessionStart();

        sub = sr_sess->onModuleChange("gnmi-server-test", module_change_cb,
                                      "/gnmi-server-test:test2/custom-error");

        sr_sess->switchDatastore(sysrepo::Datastore::Operational);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='A']", std::nullopt);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='B']", std::nullopt);
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='A']/counter", "1");
        sr_sess->setItem("/gnmi-server-test:test-state/things[name='B']/counter", "2");
        sr_sess->setItem("/gnmi-server-test:test-state/cargo", "");
        sr_sess->applyChanges();

        sub->onRPCAction("/gnmi-server-test:clear-stats", clear_stats_rpc_cb, 0,
                         sysrepo::SubscribeOptions::Default);
        sub->onRPCAction("/gnmi-server-test:action-test/action-test", action_test_cb, 0,
                         sysrepo::SubscribeOptions::Default);
    };

    ~SetupSysrepo()
    {
        SLOG_DEBUG("Removing sysrepo data\n");
        sub.reset();
        sr_sess->getConnection().removeModules({"gnmi-server-test", "gnmi-server-test-wine"},
                                               sysrepo::ModuleRemoval::WithDependencies);
        // don't hold onto sr_sess forever.
        sr_sess.reset();
    }

    // prevent copy and assignments.
    SetupSysrepo(SetupSysrepo &) = delete;
    SetupSysrepo &operator=(const SetupSysrepo &) = delete;

  private:
    std::optional<sysrepo::Subscription> sub;
};

class SetupTests
{
  public:
    ~SetupTests()
    {
        SLOG_DEBUG("Removing test directories\n");
        if (!repo_path.empty())
        {
            std::filesystem::remove_all(repo_path);
            std::filesystem::remove_all(sr_shm_path);
        }
    }

    SetupTests()
    {
        if (getenv("SYSREPO_REPOSITORY_PATH"))
        {
            // Don't set environment variables if user has already specified them!
            // They may have good reason to do it, and may know what they are doing.
            SLOG_WARN("Using user-defined sysrepo env\n\n");
            return;
        }

        repo_path = TESTS_WORKING_DIR "/repository";
        sr_shm_path = "/dev/shm/gnmi-server-test";

        if (std::filesystem::exists(repo_path))
        {
            std::filesystem::remove_all(repo_path);
        }
        if (std::filesystem::exists(sr_shm_path))
        {
            std::filesystem::remove_all(sr_shm_path);
        }

        if (setenv("SYSREPO_REPOSITORY_PATH", repo_path.c_str(), 0))
        {
            throw std::runtime_error("Failed to setenv SYSREPO_REPOSITORY_PATH " +
                                     std::string(strerror(errno)));
        }

        if (setenv("SYSREPO_SHM_DIR", sr_shm_path.c_str(), 0))
        {
            throw std::runtime_error("Failed to setenv SYSREPO_SHM_DIR " +
                                     std::string(strerror(errno)));
        }

        if (setenv("SR_ENV_RUN_TESTS", "1", 0))
        {
            throw std::runtime_error("Failed to setenv SR_ENV_RUN_TESTS " +
                                     std::string(strerror(errno)));
        }

        // Print this so debugging is easier
        SLOG_INFO("Running tests with \nSYSREPO_REPOSITORY_PATH=", repo_path,
                  "\nSYSREPO_SHM_DIR=", sr_shm_path, "\nSR_ENV_RUN_TESTS=1\n");
    }

  private:
    std::string repo_path, sr_shm_path, log_path;
};

int main(int argc, char *argv[])
{
    int result = EXIT_FAILURE;
    std::string bind_addr = "localhost:40051";
    slog::set_level(4);

    // setup signal handler only for SIGTERM, but not SIGINT,
    // because it can interfere with a user running tests under gdb
    SetupSignalHandler(false);

    SetupTests _setup_tests;

    AuthBuilder auth;
    auth.setInsecure(true);

    std::promise<void> promise;
    auto server_ready = promise.get_future();
    std::thread server_thread(RunServer, bind_addr, auth.build(), sysrepo::Connection(),
                              std::move(promise));

    SetupSysrepo _setup_sysrepo;

    // Wait for gnmi-server to be setup.
    server_ready.wait();

    client =
        gnmi::gNMI::NewStub(grpc::CreateChannel(bind_addr, grpc::InsecureChannelCredentials()));
    gnxi_client =
        gnxi::gNXI::NewStub(grpc::CreateChannel(bind_addr, grpc::InsecureChannelCredentials()));

    result = Catch::Session().run(argc, argv);

    // cannot use raise() because, according to manpage
    //     In a multithreaded program it is equivalent to
    //         pthread_kill(pthread_self(), sig);
    // and we don't want to send the signal to this thread.

    // signal gnmi_server to stop by triggering the signal handler
    kill(getpid(), SIGTERM);

    server_thread.join();

    return result;
}
