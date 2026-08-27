/**
 * @file test_main.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Main test implementation
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

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#define CATCH_CONFIG_RUNNER
#include "catch2/catch.hpp"

#include <sysrepo-cpp/Subscription.hpp>

#include "config.h"
#include "test_main.h"

std::string insecure_addr;
std::string mtls_addr;
std::unique_ptr<gnmi::gNMI::Stub> gnmi_client;
std::unique_ptr<yang_rpc::YANG_RPC::Stub> gnxi_client;
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

/**
 * @brief Apply a complete JSON configuration to the running datastore.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] file JSON configuration file to apply.
 * @param[in] sock_path Socket path to substitute for the placeholder.
 */
static void apply_config(sysrepo::Session &sess, const std::filesystem::path &file,
                         const std::string &sock_path)
{
    std::ifstream in(file);
    if (!in)
    {
        throw std::runtime_error("cannot open " + file.string());
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // replace path to the unix socket in configuration
    if (!sock_path.empty())
    {
        auto pos = data.find("@SOCKET_PATH@");
        if (pos != std::string::npos)
        {
            data.replace(pos, std::string("@SOCKET_PATH@").size(), sock_path);
        }
    }

    // apply configuration
    auto tree = sess.getContext().parseData(data, libyang::DataFormat::JSON,
                                            libyang::ParseOptions::ParseOnly |
                                                libyang::ParseOptions::NoState |
                                                libyang::ParseOptions::Strict);
    if (!tree.has_value())
    {
        throw std::runtime_error("no data parsed from " + file.string());
    }
    sess.switchDatastore(sysrepo::Datastore::Running);
    sess.editBatch(tree.value(), sysrepo::DefaultOperation::Replace);
    sess.applyChanges();
}

/**
 * @brief Create fresh repository and SHM directories for a sysrepo instance.
 *
 * @param[in] repo_path Path to the repository directory.
 * @param[in] shm_path Path to the SHM directory.
 */
static void prepare_sysrepo_dirs(const std::string &repo_path, const std::string &shm_path)
{
    if (std::filesystem::exists(shm_path))
    {
        std::filesystem::remove_all(shm_path);
    }
    if (std::filesystem::exists(repo_path))
    {
        std::filesystem::remove_all(repo_path);
    }

    // copy the YANG modules into the repository yang dir
    std::filesystem::path modules_dir = GNXI_SCHEMA_DIR;
    std::filesystem::path yang_dst_dir = std::filesystem::path(repo_path) / "yang";
    std::filesystem::create_directories(yang_dst_dir);
    for (const auto &entry : std::filesystem::directory_iterator(modules_dir))
    {
        if (entry.path().extension() == ".yang")
        {
            std::filesystem::copy(entry.path(), yang_dst_dir / entry.path().filename(),
                                  std::filesystem::copy_options::overwrite_existing);
        }
    }
}

/**
 * @brief Install all the required modules into the repository.
 *
 * @param[in] sr_conn Sysrepo connection.
 */
static void install_test_modules(sysrepo::Connection &sr_conn)
{
    std::filesystem::path files_dir{TESTS_SCHEMA_DIR};
    std::filesystem::path modules_dir{GNXI_SCHEMA_DIR};

    auto make_inst = [](const std::filesystem::path &dir, const std::string &file,
                        std::vector<std::string> features = {},
                        std::optional<mode_t> permissions = std::nullopt)
    {
        sysrepo::ModuleInstallation inst;
        inst.schema = dir / file;
        inst.features = std::move(features);
        if (permissions)
        {
            inst.permissions = *permissions;
        }
        return inst;
    };

    std::vector<sysrepo::ModuleInstallation> modules = {
        make_inst(files_dir, "gnmi-server-test.yang"),
        make_inst(files_dir, "gnmi-server-test-wine.yang"),
        make_inst(modules_dir, "iana-tls-cipher-suite-algs@2024-03-16.yang"),
        make_inst(modules_dir, "ietf-crypto-types@2024-10-10.yang",
                  {"cleartext-private-keys", "one-asymmetric-key-format"}),
        make_inst(modules_dir, "ietf-keystore@2024-10-10.yang",
                  {"central-keystore-supported", "asymmetric-keys"}),
        make_inst(modules_dir, "ietf-truststore@2024-10-10.yang",
                  {"central-truststore-supported", "certificates"}),
        make_inst(modules_dir, "ietf-tls-common@2024-10-10.yang"),
        make_inst(modules_dir, "ietf-tls-server@2024-10-10.yang",
                  {"server-ident-x509-cert", "client-auth-supported", "client-auth-x509-cert"}),
        make_inst(modules_dir, "sysrepo-gnxi-server.yang", {}, 0600),
        make_inst(modules_dir, "sysrepo-gnxi-users.yang", {}, 0600),
    };
    sr_conn.installModules(modules, {files_dir, modules_dir});
}

class SetupServer
{
  public:
    /**
     * @brief Launch the server subprocess for this test binary.
     *
     * @param[in] test_name Name of the test (for per-binary paths).
     * @param[in] debug Wait for the user before starting the server.
     * @param[in] secure Launch the secure (mTLS) server instead of the
     *                   insecure one.
     */
    SetupServer(const std::string &test_name, bool debug = false, bool secure = false)
        : debug_(debug), secure_(secure)
    {
        if (secure_)
        {
            // the secure server reads its whole configuration (including
            // TLS) from sysrepo, so it must be written to the repository
            // before the launch
            log_ = std::filesystem::path(TESTS_WORKING_DIR) / (test_name + "-mtls.log");
            std::filesystem::remove(log_);
            mtls_addr = std::string(HOST) + ":" + std::to_string(PORT);

            apply_config(*sr_sess, std::filesystem::path(TESTS_SCHEMA_DIR) / "secure.json", "");
            server_pid_ = run_server({"-l", "4"}, log_.string(), "mTLS");
        }
        else
        {
            // insecure server - per-binary paths so tests can run in parallel
            sock_ = std::filesystem::path(TESTS_WORKING_DIR) / (test_name + ".sock");
            log_ = std::filesystem::path(TESTS_WORKING_DIR) / (test_name + "-insecure.log");
            std::filesystem::remove(sock_);
            std::filesystem::remove(log_);
            insecure_addr = "unix:" + sock_.string();

            apply_config(*sr_sess, std::filesystem::path(TESTS_SCHEMA_DIR) / "insecure.json",
                         sock_.string());
            server_pid_ = run_server({"-f", "-l", "4"}, log_.string(), "Insecure");
        }
    }

    /**
     * @brief Wait for the launched server to become ready.
     *
     * @throws std::runtime_error if the server does not start listening
     *         within the timeout.
     */
    void wait_ready()
    {
        if (secure_)
        {
            wait_for_tcp(HOST, PORT);
        }
        else
        {
            wait_for_unix(sock_.string());
        }
    }

    ~SetupServer()
    {
        stop(server_pid_);
        // the log is kept
    }

  private:
    pid_t server_pid_ = -1;
    std::filesystem::path sock_;
    std::filesystem::path log_;
    static constexpr const char *HOST = "127.0.0.1";
    static constexpr uint16_t PORT = 50052;
    bool debug_;
    bool secure_;

    /**
     * @brief Stop process by killing it and waiting for it.
     *
     * @param[in] pid Id of a process to kill.
     */
    static void stop(pid_t pid)
    {
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            int status;
            waitpid(pid, &status, 0);
        }
    }

    /**
     * @brief Create a separate process and run server on it.
     *
     * @param[in] args Arguments for the server.
     * @param[in] log_path Log path for server's stdout/stderr.
     * @param[in] label Custom server label.
     * @return New process id.
     */
    pid_t run_server(const std::vector<std::string> &args, const std::string &log_path,
                     const char *label)
    {
        int pipefd[2] = {-1, -1};
        if (debug_)
        {
            if (pipe(pipefd) < 0)
            {
                throw std::runtime_error("pipe failed");
            }
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            throw std::runtime_error("fork failed");
        }

        if (pid == 0)
        {
            // child
            if (debug_)
            {
                close(pipefd[1]);
                // wait for parent's signal before exec
                char buf = 0;
                if (read(pipefd[0], &buf, 1) != 1)
                {
                    _exit(127);
                }
                close(pipefd[0]);
            }

            int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0)
            {
                _exit(127);
            }
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);

            std::vector<const char *> argv;
            argv.push_back(SERVER_BINARY);
            for (const auto &a : args)
            {
                argv.push_back(a.c_str());
            }
            argv.push_back(nullptr);

            execv(argv[0], const_cast<char *const *>(argv.data()));
            _exit(127);
        }

        // parent
        if (debug_)
        {
            close(pipefd[0]);
            fprintf(stderr, "%s server PID: %d\n", label, pid);
            fprintf(stderr, "Attach gdb: gdb -p %d\n", pid);
            fprintf(stderr, "Press Enter to continue...\n");
            getc(stdin);
            char buf = 0;
            if (write(pipefd[1], &buf, 1) != 1)
            {
                perror("write");
                stop(pid);
                throw std::runtime_error("failed to signal child");
            }
            close(pipefd[1]);
        }

        return pid;
    }

    /**
     * @brief Wait for server to start listening on the unix socket.
     *
     * @param[in] sock_path Socket to check.
     */
    static void wait_for_unix(const std::string &sock_path)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock >= 0)
            {
                struct sockaddr_un addr = {};
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
                if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
                {
                    close(sock);
                    return;
                }
                close(sock);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("Insecure server (unix socket) did not become ready within 10s");
    }

    /**
     * @brief Wait for server to start listening on specific port on host.
     *
     * @param[in] host Host address.
     * @param[in] port Host port.
     */
    static void wait_for_tcp(const std::string &host, uint16_t port)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            // poll TCP connect to make sure the port is actually accepting
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0)
            {
                struct sockaddr_in sin = {};
                sin.sin_family = AF_INET;
                sin.sin_port = htons(port);
                if (inet_pton(AF_INET, host.c_str(), &sin.sin_addr) != 1)
                {
                    throw std::runtime_error("invalid binding address for mTLS server");
                }
                if (connect(sock, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) == 0)
                {
                    close(sock);
                    return;
                }
                close(sock);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("mTLS server did not become ready within 10s");
    }
};

class SetupSysrepo
{
  public:
    SetupSysrepo()
    {
        auto sr_conn = sysrepo::Connection();
        install_test_modules(sr_conn);

        sr_sess = sr_conn.sessionStart();

        sub = sr_sess->onModuleChange("gnmi-server-test", module_change_cb,
                                      "/gnmi-server-test:test2/custom-error");

        // operational data for the Get/Subscribe tests
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
        sub.reset();
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
    SetupTests(const std::string &test_name)
    {
        if (getenv("SYSREPO_REPOSITORY_PATH"))
        {
            // Don't set environment variables if user has already specified them!
            // They may have good reason to do it, and may know what they are doing.
            fprintf(stderr, "Using user-defined sysrepo env\n\n");
            return;
        }

        // namespace per-binary so tests can run in parallel
        repo_path = std::string(TESTS_WORKING_DIR) + "/" + test_name + "-repository";
        sr_shm_path = std::string(TESTS_WORKING_DIR) + "/" + test_name + "-shm";
        prepare_sysrepo_dirs(repo_path, sr_shm_path);

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
        fprintf(stderr,
                "Running tests with \nSYSREPO_REPOSITORY_PATH=%s\n"
                "SYSREPO_SHM_DIR=%s\nSR_ENV_RUN_TESTS=1\n",
                repo_path.c_str(), sr_shm_path.c_str());
    }

  private:
    std::string repo_path, sr_shm_path;
};

int main(int argc, char *argv[])
{
    int result = EXIT_FAILURE;
    bool debug = false;

    // strip --debug from argv before passing to Catch2
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--debug")
        {
            debug = true;
            // shift remaining args left
            for (int j = i; j < argc - 1; j++)
            {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        }
    }

    // get test name to create unique paths so tests can run in parallel
    std::filesystem::path test_path(argv[0]);
    std::string test_name = test_path.stem().string();

    try
    {
        SetupTests _setup_tests(test_name);
        SetupSysrepo _setup_sysrepo;

#ifdef AUTH_MTLS_ENABLED
        // this binary tests the secure (mTLS) server
        SetupServer _setup_server(test_name, debug, true);
        _setup_server.wait_ready();
#else
        SetupServer _setup_server(test_name, debug, false);
        _setup_server.wait_ready();

        auto main_channel = grpc::CreateChannel(insecure_addr, grpc::InsecureChannelCredentials());
        main_channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(10));
        gnmi_client = gnmi::gNMI::NewStub(main_channel);
#ifdef GNXI_SERVICE_ENABLED
        gnxi_client = yang_rpc::YANG_RPC::NewStub(main_channel);
#endif
#endif

        result = Catch::Session().run(argc, argv);
    }
    catch (const std::exception &exc)
    {
        fprintf(stderr, "Test setup failed: %s\n", exc.what());
        result = EXIT_FAILURE;
    }

    return result;
}
