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
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/DataNode.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>

#include "config.h"

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;
using Catch::Matchers::Matches;

/**
 * @brief Fixture: removes all temporary files and provides helpers for running and testing the
 * sysrepo-gnxi-users binary against the sysrepo user database.
 *
 */
class UsersFixture
{
  public:
    static std::filesystem::path log;

    UsersFixture() = default;

    ~UsersFixture() { cleanup(); }

    /**
     * @brief Remove the log and all user/NACM data manipulated by the tests.
     *
     */
    void cleanup()
    {
        std::filesystem::remove_all(log);
        // clean up any existing user data in sysrepo
        try
        {
            auto sess = sysrepo::Connection().sessionStart(sysrepo::Datastore::Running);
            sess.deleteItem("/sysrepo-gnxi-users:users");
            sess.deleteItem("/ietf-netconf-acm:nacm/groups");
            sess.deleteItem("/ietf-netconf-acm:nacm/rule-list");
            sess.applyChanges();
        }
        catch (...)
        {
        }
    }

    /**
     * @brief Read the CLI log of the last run.
     *
     * @return CLI output from the log file.
     */
    std::string read_log()
    {
        std::ifstream ifs(log);
        return {std::istreambuf_iterator<char>(ifs), {}};
    }

    /**
     * @brief Add a username to a NACM group directly in sysrepo.
     *
     * @param[in] group NACM group to add the membership to.
     * @param[in] name Username to add as the group member.
     */
    void nacm_group_add(const std::string &group, const std::string &name)
    {
        auto sess = sysrepo::Connection().sessionStart(sysrepo::Datastore::Running);
        sess.setItem(
            "/ietf-netconf-acm:nacm/groups/group[name='" + group + "']/user-name", name.c_str());
        sess.applyChanges();
    }

    /**
     * @brief Check whether a username is a member of a NACM group.
     *
     * @param[in] group NACM group to check.
     * @param[in] name Username to look for.
     * @return True if the membership exists.
     */
    bool nacm_group_has(const std::string &group, const std::string &name)
    {
        auto sess = sysrepo::Connection().sessionStart(sysrepo::Datastore::Running);
        auto data = sess.getData("/ietf-netconf-acm:nacm/groups/group[name='" + group + "']");
        if (!data.has_value())
        {
            return false;
        }
        for (auto entry : data->findXPath("/ietf-netconf-acm:nacm/groups/group/user-name"))
        {
            if (entry.asTerm().valueStr() == name)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Run the sysrepo-gnxi-users CLI binary as a subprocess.
     *
     * @param[in] args Arguments for the binary.
     * @return Exit status of the subprocess, -1 on fork failure or abnormal exit.
     */
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

    /**
     * @brief Get user database root node from sysrepo.
     *
     * @return Root node of the user database.
     */
    libyang::DataNode get_db(void)
    {
        auto sess = sysrepo::Connection().sessionStart(sysrepo::Datastore::Running);
        auto data = sess.getData("/sysrepo-gnxi-users:users");
        if (!data.has_value())
        {
            throw std::runtime_error("no user database in sysrepo");
        }
        return std::move(data.value());
    }

    /**
     * @brief Get user with a specific name.
     *
     * @param[in] root User database root node.
     * @param[in] name Name of the user to find.
     * @return User entry with the given name.
     */
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

    /**
     * @brief Get password of a specific user.
     *
     * @param[in] user User entry to read from.
     * @return Password of the user.
     */
    std::string get_password(libyang::DataNode &user)
    {
        auto password = user.findPath("password");
        if (!password.has_value())
        {
            throw std::runtime_error("user has no password leaf");
        }
        return password->asTerm().valueStr();
    }

    /**
     * @brief Check if any users exist in the database.
     *
     * @return True if there are no users in the database, false otherwise.
     */
    bool db_empty(void)
    {
        try
        {
            auto root = get_db();
            auto users = root.findXPath("/sysrepo-gnxi-users:users/user");
            for (auto u : users)
            {
                (void)u;
                return false;
            }
            return true;
        }
        catch (...)
        {
            return true;
        }
    }
};

std::filesystem::path UsersFixture::log =
    std::filesystem::path(TESTS_WORKING_DIR) / "test-users-cli.log";

// add

TEST_CASE_METHOD(UsersFixture, "Users: add user with default (sha512) hash", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "secret123"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$6\\$[a-zA-Z0-9./]{16}\\$[a-zA-Z0-9./]{86}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (sha512) password", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "bob", "--password", "bobpass", "--hash", "sha512"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "bob");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$6\\$[a-zA-Z0-9./]{16}\\$[a-zA-Z0-9./]{86}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (sha256) password", "[users]")
{
    REQUIRE(
        run_bin({"add", "--name", "carol", "--password", "carolpass", "--hash", "sha256"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "carol");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$5\\$[a-zA-Z0-9./]{16}\\$[a-zA-Z0-9./]{43}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with hashed (md5) password", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "dave", "--password", "davepass", "--hash", "md5"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "dave");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$1\\$[a-zA-Z0-9./]{8}\\$[a-zA-Z0-9./]{22}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: add user with unknown hash algorithm fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "eve", "--password", "evepass", "--hash", "sha1"}) != 0);
    CHECK(db_empty());
}

TEST_CASE_METHOD(UsersFixture, "Users: add duplicate user fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass1"}) == 0);
    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password1 = get_password(user);

    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass2"}) != 0);
    root = get_db();
    user = get_user(root, "alice");
    auto password2 = get_password(user);
    CHECK_THAT(password1, Equals(password2));
}

// edit

TEST_CASE_METHOD(UsersFixture, "Users: edit password", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "oldpass", "--hash", "sha512"}) == 0);
    REQUIRE(run_bin({"edit", "--name", "alice", "--password", "newpass", "--hash", "sha256"}) == 0);

    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$5\\$[a-zA-Z0-9./]{16}\\$[a-zA-Z0-9./]{43}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: edit password with default algorithm", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "oldpass", "--hash", "md5"}) == 0);
    auto root = get_db();
    auto user = get_user(root, "alice");
    auto password1 = get_password(user);

    REQUIRE(run_bin({"edit", "--name", "alice", "--password", "newpass"}) == 0);
    root = get_db();
    user = get_user(root, "alice");
    auto password2 = get_password(user);
    CHECK_THAT(password1, !Equals(password2));
}

TEST_CASE_METHOD(UsersFixture, "Users: edit nonexistent user fails", "[users]")
{
    REQUIRE(run_bin({"edit", "--name", "ghost", "--password", "pass"}) != 0);
    CHECK(db_empty());
}

TEST_CASE_METHOD(UsersFixture, "Users: edit with nothing to edit fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"edit", "--name", "alice"}) != 0);
}

// remove

TEST_CASE_METHOD(UsersFixture, "Users: remove user", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass1"}) == 0);
    REQUIRE(run_bin({"add", "--name", "bob", "--password", "pass2"}) == 0);
    REQUIRE(run_bin({"remove", "--name", "alice"}) == 0);

    auto root = get_db();
    CHECK_THROWS(get_user(root, "alice"));
    auto user = get_user(root, "bob");
    auto password = get_password(user);
    CHECK_THAT(password, Matches("^\\$6\\$[a-zA-Z0-9./]{16}\\$[a-zA-Z0-9./]{86}$"));
}

TEST_CASE_METHOD(UsersFixture, "Users: remove keeps NACM groups by default", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    nacm_group_add("admin", "alice");
    nacm_group_add("readonly", "alice");

    REQUIRE(run_bin({"remove", "--name", "alice"}) == 0);
    auto root = get_db();
    CHECK_THROWS(get_user(root, "alice"));

    // dormant NACM grants are the RFC standard behavior, no cleanup by default
    CHECK(nacm_group_has("admin", "alice"));
    CHECK(nacm_group_has("readonly", "alice"));
}

TEST_CASE_METHOD(UsersFixture, "Users: remove --purge-nacm strips NACM groups", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"add", "--name", "bob", "--password", "pass"}) == 0);
    nacm_group_add("admin", "alice");
    nacm_group_add("readonly", "alice");
    nacm_group_add("admin", "bob");

    REQUIRE(run_bin({"remove", "--name", "alice", "--purge-nacm"}) == 0);
    auto root = get_db();
    CHECK_THROWS(get_user(root, "alice"));

    CHECK(!nacm_group_has("admin", "alice"));
    CHECK(!nacm_group_has("readonly", "alice"));
    // memberships of other users are untouched
    CHECK(nacm_group_has("admin", "bob"));
    // and the purge is reported in the CLI log
    auto output = read_log();
    CHECK_THAT(output, Contains("Removed 'alice' from NACM group 'admin'"));
    CHECK_THAT(output, Contains("Removed 'alice' from NACM group 'readonly'"));
}

TEST_CASE_METHOD(UsersFixture, "Users: remove nonexistent user fails", "[users]")
{
    REQUIRE(run_bin({"remove", "--name", "ghost"}) != 0);
    CHECK(db_empty());
}

TEST_CASE_METHOD(UsersFixture, "Users: --purge-nacm applies only to remove", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass", "--purge-nacm"}) != 0);
    CHECK(db_empty());
    REQUIRE(run_bin({"show", "--purge-nacm"}) != 0);
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"edit", "--name", "alice", "--password", "x", "--purge-nacm"}) != 0);
}

// show

TEST_CASE_METHOD(UsersFixture, "Users: show nonexistent user fails", "[users]")
{
    REQUIRE(run_bin({"show", "--name", "ghost"}) != 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: show shows NACM group memberships", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"add", "--name", "bob", "--password", "pass"}) == 0);
    nacm_group_add("admin", "alice");
    nacm_group_add("readonly", "alice");

    REQUIRE(run_bin({"show", "--name", "alice"}) == 0);
    auto output = read_log();
    CHECK_THAT(output, Contains("admin"));
    CHECK_THAT(output, Contains("readonly"));

    // a user with no memberships says so
    REQUIRE(run_bin({"show", "--name", "bob"}) == 0);
    CHECK_THAT(read_log(), Contains("no rules apply"));
}

TEST_CASE_METHOD(UsersFixture, "Users: show lists all users", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"add", "--name", "bob", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"show"}) == 0);
    auto output = read_log();
    CHECK_THAT(output, Contains("alice"));
    CHECK_THAT(output, Contains("bob"));
}

TEST_CASE_METHOD(UsersFixture, "Users: show rejects irrelevant options", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"show", "--password", "pass"}) != 0);
    REQUIRE(run_bin({"show", "--hash", "sha512"}) != 0);
}

// error handling

TEST_CASE_METHOD(UsersFixture, "Users: missing --name fails", "[users]")
{
    REQUIRE(run_bin({"add", "--password", "pass"}) != 0);
    CHECK(db_empty());
}

TEST_CASE_METHOD(UsersFixture, "Users: add without --password fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice"}) != 0);
    CHECK(db_empty());
}

TEST_CASE_METHOD(UsersFixture, "Users: --hash without --password (in edit) fails", "[users]")
{
    REQUIRE(run_bin({"add", "--name", "alice", "--password", "pass"}) == 0);
    REQUIRE(run_bin({"edit", "--name", "alice", "--hash", "sha512"}) != 0);
}

TEST_CASE_METHOD(UsersFixture, "Users: unknown command fails", "[users]")
{
    REQUIRE(run_bin({"bogus"}) != 0);
    CHECK(db_empty());
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

TEST_CASE_METHOD(UsersFixture, "Users: storing a '$0$' plaintext password is rejected", "[users]")
{
    auto sess = sysrepo::Connection().sessionStart(sysrepo::Datastore::Running);
    sess.setItem("/sysrepo-gnxi-users:users/user[name='eve']/password", "$0$plaintext");
    CHECK_THROWS(sess.applyChanges());
    CHECK(db_empty());
}
