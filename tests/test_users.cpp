/**
 * @file test_users.cpp
 * @author Ondrej Kusnirik <kusnirik@cesnet.cz>
 * @brief Users command-line utility tests
 *
 * @copyright
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

#include "catch2/catch.hpp"

#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/DataNode.hpp>
#include <libyang/parser_schema.h>

#include "config.h"

using Catch::Matchers::Matches;

/**
 * @brief Fixture: removes all temporary files and provides helpers for running and testing the
 * sysrepo-gnxi-users binary and the JSON file database.
 *
 */
class UsersFixture
{
  public:
    static std::filesystem::path log;
    static std::filesystem::path db;
    UsersFixture()
    {
        std::filesystem::remove_all(log);
        std::filesystem::remove_all(db);
    };
    ~UsersFixture()
    {
        std::filesystem::remove_all(log);
        std::filesystem::remove_all(db);
    };

    // run the sysrepo-gnxi-users CLI binary as a subprocess
    int run_bin(const std::vector<std::string> &args)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            return -1;
        }
        if (pid == 0)
        {
            // redirect stdout/stderr to a log file in the build dir
            int fd = open(UsersFixture::log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0)
            {
                _exit(127);
            }
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);

            // binary path + args + nullptr
            std::vector<const char *> argv;
            argv.push_back(USERS_BINARY);
            for (const auto &a : args)
            {
                argv.push_back(a.c_str());
            }
            argv.push_back(nullptr);

            execv(argv[0], const_cast<char *const *>(argv.data()));
            _exit(127);
        }

        // wait for child
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        return -1;
    }

    // get user database root node
    libyang::DataNode get_db(void)
    {
        libyang::Context ctx(ly_yang_module_dir(), libyang::ContextOptions::NoYangLibrary);
        ctx.parseModule(std::filesystem::path(GNXI_SCHEMA_DIR) / "sysrepo-gnxi-users.yang",
                        libyang::SchemaFormat::YANG);
        auto tree = ctx.parseData(UsersFixture::db, libyang::DataFormat::JSON);
        if (!tree.has_value())
        {
            throw std::runtime_error("no data in " + UsersFixture::db.string());
        }
        return std::move(tree.value());
    }

    // get user with a specific name
    libyang::DataNode get_user(libyang::DataNode &root, const std::string &name)
    {
        for (auto user : root.findXPath("/sysrepo-gnxi-users:users/user"))
        {
            auto n = user.findPath("name");
            if (n.has_value() && n->asTerm().valueStr() == name)
            {
                return user;
            }
        }
        throw std::runtime_error("no user '" + name + "' in database");
    }

    // get password of a specific user
    std::string get_password(libyang::DataNode &user)
    {
        auto password = user.findPath("password");
        if (!password.has_value())
        {
            throw std::runtime_error("user has no password leaf");
        }
        return password->asTerm().valueStr();
    }
};

std::filesystem::path UsersFixture::log =
    std::filesystem::path(TESTS_WORKING_DIR) / "test-users-cli.log";
std::filesystem::path UsersFixture::db =
    std::filesystem::path(TESTS_WORKING_DIR) / "test-users-db.json";

// add

TEST_CASE_METHOD(UsersFixture, "Users: add user with plaintext password", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "secret123"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK(password == "plaintext$secret123");
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (sha512) password", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "bob", "--password", "bobpass", "--hash",
                     "sha512"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "bob");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^sha512\\$[0-9A-F]{32}\\$[0-9A-F]{128}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (sha256) password", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "carol", "--password", "carolpass", "--hash",
                     "sha256"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "carol");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^sha256\\$[0-9A-F]{32}\\$[0-9A-F]{64}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (sha2 alias) password", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "dave", "--password", "davepass", "--hash",
                     "sha2"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "dave");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^sha2\\$[0-9A-F]{32}\\$[0-9A-F]{64}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add duplicate user fails", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass1"}) == 0);
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass2"}) != 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK(password == "plaintext$pass1");
}

TEST_CASE_METHOD(UsersFixture, "Users: add with ACL", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--hash", "sha512",
                     "--rw", "gnmi-server-test", "--ro", "ietf-interfaces"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");

    auto acl = user.findXPath("acl");
    REQUIRE(acl.size() == 2);

    bool found_rw = false, found_ro = false;
    for (auto entry : acl)
    {
        auto mod = entry.findPath("module");
        auto acc = entry.findPath("access");
        REQUIRE(mod.has_value());
        REQUIRE(acc.has_value());
        if (mod->asTerm().valueStr() == "gnmi-server-test" && acc->asTerm().valueStr() == "rw")
        {
            found_rw = true;
        }
        else if (mod->asTerm().valueStr() == "ietf-interfaces" && acc->asTerm().valueStr() == "ro")
        {
            found_ro = true;
        }
    }
    CHECK(found_rw);
    CHECK(found_ro);
}

// edit

TEST_CASE_METHOD(UsersFixture, "Users: edit password", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "oldpass", "--hash",
                     "sha512"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--password", "newpass", "--hash",
                     "sha256"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^sha256\\$[0-9A-F]{32}\\$[0-9A-F]{64}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: edit password to plaintext", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "oldpass", "--hash",
                     "sha512"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--password", "newpass"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK(password == "plaintext$newpass");
}

TEST_CASE_METHOD(UsersFixture, "Users: edit ACL", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--hash", "sha512",
                     "--rw", "mod-a"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--ro", "mod-b,mod-c"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");

    auto acl = user.findXPath("acl");
    REQUIRE(acl.size() == 3);
}

TEST_CASE_METHOD(UsersFixture, "Users: edit nonexistent user fails", "[users]")
{
    REQUIRE(run_bin({"edit", "--db", db, "--name", "ghost", "--password", "pass"}) != 0);
    CHECK(!std::filesystem::exists(db));
}

TEST_CASE_METHOD(UsersFixture, "Users: edit with nothing to edit fails", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice"}) != 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: edit --rm single module", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--rw",
                     "mod-a,mod-b,mod-c"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--rm", "mod-b"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    REQUIRE(user.findXPath("acl").size() == 2);
    CHECK(!user.findPath("acl[module='mod-b']").has_value());
}

TEST_CASE_METHOD(UsersFixture, "Users: edit --rm multiple modules", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--rw",
                     "mod-a,mod-b", "--ro", "mod-c"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--rm", "mod-a,mod-c"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    REQUIRE(user.findXPath("acl").size() == 1);
    CHECK(user.findPath("acl[module='mod-b']").has_value());
}

TEST_CASE_METHOD(UsersFixture, "Users: edit --rm then --ro (order matters)", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--rw",
                     "mod-a"}) == 0);
    // remove mod-a, then add it back as ro - the remove runs first
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--rm", "mod-a", "--ro", "mod-a"}) ==
            0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    REQUIRE(user.findXPath("acl").size() == 1);
    auto entry = user.findPath("acl[module='mod-a']");
    REQUIRE(entry.has_value());
    auto access = entry->findPath("access");
    REQUIRE(access.has_value());
    CHECK(access->asTerm().valueStr() == "ro");
}

TEST_CASE_METHOD(UsersFixture, "Users: edit --rm nonexistent module is a no-op", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--rw",
                     "mod-a"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--rm", "mod-zzz"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    REQUIRE(user.findXPath("acl").size() == 1);
}

TEST_CASE_METHOD(UsersFixture, "Users: --rm in add mode is a no-op", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass", "--rm", "mod-b",
                     "--rw", "mod-b"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    REQUIRE(user.findXPath("acl").size() == 1);
    CHECK(user.findPath("acl[module='mod-b']").has_value());
}

// remove

TEST_CASE_METHOD(UsersFixture, "Users: remove user", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass1"}) == 0);
    REQUIRE(run_bin({"add", "--db", db, "--name", "bob", "--password", "pass2"}) == 0);
    REQUIRE(run_bin({"remove", "--db", db, "--name", "alice"}) == 0);

    auto root = get_db();
    CHECK_THROWS(get_user(root, "alice"));
    auto user = get_user(root, "bob");
    auto password = get_password(user);
    CHECK(password == "plaintext$pass2");
}

TEST_CASE_METHOD(UsersFixture, "Users: remove nonexistent user fails", "[users]")
{
    REQUIRE(run_bin({"remove", "--db", db, "--name", "ghost"}) != 0);
    CHECK(!std::filesystem::exists(db));
}

// error handling

TEST_CASE_METHOD(UsersFixture, "Users: missing --db fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) != 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: missing --name fails", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--password", "pass"}) != 0);
    CHECK(!std::filesystem::exists(db));
}

TEST_CASE_METHOD(UsersFixture, "Users: add without --password fails", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice"}) != 0);
    CHECK(!std::filesystem::exists(db));
}

TEST_CASE_METHOD(UsersFixture, "Users: --hash without --password (in edit) fails", "[users]")
{
    REQUIRE(run_bin({"add", "--db", db, "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"edit", "--db", db, "--name", "alice", "--hash", "sha512"}) != 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: unknown command fails", "[users]")
{
    REQUIRE(run_bin({"bogus", "--db", db}) != 0);
    CHECK(!std::filesystem::exists(db));
}

TEST_CASE_METHOD(UsersFixture, "Users: --help exits 0", "[users]")
{
    REQUIRE(run_bin({"--help"}) == 0);
    REQUIRE(run_bin({"help"}) == 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: no arguments shows usage and fails", "[users]")
{
    REQUIRE(run_bin({}) != 0);
}
