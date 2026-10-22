/**
 * @file users.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Users command-line utility implementation
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

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <libyang-cpp/DataNode.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>

#include <openssl/crypto.h>

#include "security/hash.h"

constexpr const char *USAGE = R"(Usage:
  sysrepo-gnxi-users add --name NAME --password PASS [--hash ALGO]
  sysrepo-gnxi-users edit --name NAME --password PASS [--hash ALGO]
  sysrepo-gnxi-users remove --name NAME [--purge-nacm]
  sysrepo-gnxi-users show [--name NAME]

Options:
  -n,--name NAME        username
  -p,--password PASS    password
  -s,--hash ALGO        hashing algorithm: md5, sha256 or sha512 (default)
  --purge-nacm          with remove also strip the username from every
                        ietf-netconf-acm group

The user database is stored in sysrepo under
/sysrepo-gnxi-users:users and is protected by filesystem permissions.
Authorization of the users is configured in /ietf-netconf-acm (NACM).
)";

/**
 * @brief Options for user creation/edit/deletion in the user database.
 *
 */
struct Options
{
    std::string command, name, password, hash;
    bool purge_nacm = false;
};

/**
 * @brief Parse command line options.
 *
 * @param[in] argc Number of command line arguments.
 * @param[in] argv Command line arguments.
 * @return Options for database manipulation.
 */
Options parse_args(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << USAGE;
        std::exit(-1);
    }

    Options opts;
    opts.command = argv[1];
    if (opts.command == "--help" || opts.command == "-h" || opts.command == "help")
    {
        std::cout << USAGE;
        std::exit(0);
    }
    if (opts.command != "add" && opts.command != "edit" && opts.command != "remove" &&
        opts.command != "show")
    {
        std::cerr << "Unknown command: " << opts.command << "\n\n" << USAGE;
        std::exit(-1);
    }

    static struct option long_options[] = {{"name", required_argument, 0, 'n'},
        {"password", required_argument, 0, 'p'}, {"hash", required_argument, 0, 's'},
        {"purge-nacm", no_argument, 0, 0}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};

    optind = 2; // skip program name and command
    int c;
    int purge_nacm_index = -1;
    while ((c = getopt_long(argc, argv, "n:p:s:h", long_options, &purge_nacm_index)) != -1)
    {
        switch (c)
        {
        case 0:
            // long option without a short equivalent (purge-nacm is the only one)
            opts.purge_nacm = true;
            break;
        case 'n':
            opts.name = optarg;
            break;
        case 'p':
            opts.password = optarg;
            break;
        case 's':
            opts.hash = optarg;
            break;
        case 'h':
            std::cout << USAGE;
            std::exit(0);
        default:
            std::cerr << USAGE;
            std::exit(-1);
        }
    }

    if (opts.purge_nacm && opts.command != "remove")
    {
        std::cerr << "--purge-nacm applies only to the remove command\n\n" << USAGE;
        std::exit(-1);
    }

    if (opts.command == "show")
    {
        // show accepts only an optional --name
        if (!opts.password.empty() || !opts.hash.empty())
        {
            std::cerr << "show accepts only an optional --name\n\n" << USAGE;
            std::exit(-1);
        }
        return opts;
    }

    if (opts.name.empty())
    {
        std::cerr << "Missing --name\n\n" << USAGE;
        std::exit(-1);
    }
    if (opts.command == "add" && opts.password.empty())
    {
        std::cerr << "add requires --password\n\n" << USAGE;
        std::exit(-1);
    }
    if (opts.command == "edit" && !opts.hash.empty() && opts.password.empty())
    {
        std::cerr << "--hash requires --password\n\n" << USAGE;
        std::exit(-1);
    }
    if (opts.command == "edit" && opts.password.empty())
    {
        std::cerr << "Nothing to edit: provide --password\n\n" << USAGE;
        std::exit(-1);
    }

    return opts;
}

/**
 * @brief Check if a user exists in the sysrepo user database.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] name Username to check.
 * @return True if user exists.
 */
bool user_exists(sysrepo::Session &sess, const std::string &name)
{
    auto data = sess.getData("/sysrepo-gnxi-users:users/user[name='" + name + "']");
    return data.has_value();
}

/**
 * @brief Add a new user to the database.
 *
 * @param[in] opts Command line options.
 */
void cmd_add(const Options &opts)
{
    sysrepo::Connection conn;
    auto sess = conn.sessionStart(sysrepo::Datastore::Running);

    if (user_exists(sess, opts.name))
    {
        throw std::runtime_error("User '" + opts.name + "' already exists");
    }

    std::string user_path = "/sysrepo-gnxi-users:users/user[name='" + opts.name + "']";
    sess.setItem(user_path + "/password", make_hash(opts.password, opts.hash));
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' added to sysrepo\n";
}

/**
 * @brief Edit user's password in the database.
 *
 * @param[in] opts Command line options.
 */
void cmd_edit(const Options &opts)
{
    sysrepo::Connection conn;
    auto sess = conn.sessionStart(sysrepo::Datastore::Running);

    if (!user_exists(sess, opts.name))
    {
        throw std::runtime_error("User '" + opts.name + "' not found in sysrepo");
    }

    std::string user_path = "/sysrepo-gnxi-users:users/user[name='" + opts.name + "']";
    sess.setItem(user_path + "/password", make_hash(opts.password, opts.hash));
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' updated in sysrepo\n";
}

/**
 * @brief Strip the username from every ietf-netconf-acm group it is a member
 * of, printing each removed membership.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] name Username to purge.
 */
void purge_nacm(sysrepo::Session &sess, const std::string &name)
{
    auto groups = sess.getData("/ietf-netconf-acm:nacm/groups");
    if (!groups.has_value())
    {
        return;
    }

    for (auto group : groups->findXPath("/ietf-netconf-acm:nacm/groups/group"))
    {
        auto gname = group.findPath("name");
        if (!gname.has_value())
        {
            continue;
        }
        for (auto entry : group.findXPath("user-name"))
        {
            if (entry.asTerm().valueStr() != name)
            {
                continue;
            }
            sess.deleteItem(entry.path());
            std::cout << "Removed '" << name << "' from NACM group '" << gname->asTerm().valueStr()
                      << "'\n";
        }
    }
}

/**
 * @brief Remove user from the database.
 *
 * @param[in] opts Command line options.
 */
void cmd_remove(const Options &opts)
{
    sysrepo::Connection conn;
    auto sess = conn.sessionStart(sysrepo::Datastore::Running);

    if (!user_exists(sess, opts.name))
    {
        throw std::runtime_error("User '" + opts.name + "' not found in sysrepo");
    }

    sess.deleteItem("/sysrepo-gnxi-users:users/user[name='" + opts.name + "']");
    if (opts.purge_nacm)
    {
        purge_nacm(sess, opts.name);
    }
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' removed from sysrepo\n";
}

/**
 * @brief Collect all usernames from sysrepo.
 *
 * @param[in] sess Sysrepo session.
 * @return Usernames sorted alphabetically.
 */
std::vector<std::string> collect_users(sysrepo::Session &sess)
{
    std::vector<std::string> users;
    auto data = sess.getData("/sysrepo-gnxi-users:users");
    if (data.has_value())
    {
        for (auto user : data->findXPath("/sysrepo-gnxi-users:users/user"))
        {
            auto uname = user.findPath("name");
            if (!uname.has_value())
            {
                continue;
            }
            users.push_back(uname->asTerm().valueStr());
        }
    }
    std::sort(users.begin(), users.end());
    return users;
}

/**
 * @brief Collect the NACM groups a username is a member of (read-only
 * convenience display of /ietf-netconf-acm:nacm/groups).
 *
 * @param[in] sess Sysrepo session.
 * @param[in] name Username to look up.
 * @return NACM group names the user is a member of, sorted alphabetically.
 */
std::vector<std::string> collect_nacm_groups(sysrepo::Session &sess, const std::string &name)
{
    std::vector<std::string> groups;
    auto data = sess.getData("/ietf-netconf-acm:nacm/groups");
    if (data.has_value())
    {
        for (auto group : data->findXPath("/ietf-netconf-acm:nacm/groups/group"))
        {
            for (auto entry : group.findXPath("user-name"))
            {
                if (entry.asTerm().valueStr() == name)
                {
                    auto gname = group.findPath("name");
                    if (!gname.has_value())
                    {
                        continue;
                    }
                    groups.push_back(gname->asTerm().valueStr());
                    break;
                }
            }
        }
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

/**
 * @brief Show all users, or the details of a specific user.
 *
 * @param[in] opts Command line options.
 */
void cmd_show(const Options &opts)
{
    sysrepo::Connection conn;
    auto sess = conn.sessionStart(sysrepo::Datastore::Running);
    auto users = collect_users(sess);

    // a specific user requested
    if (!opts.name.empty())
    {
        if (std::find(users.begin(), users.end(), opts.name) == users.end())
        {
            throw std::runtime_error("User '" + opts.name + "' not found in sysrepo");
        }
        std::cout << "--- " << opts.name << "'s NACM groups " << " ---\n";

        auto groups = collect_nacm_groups(sess, opts.name);
        if (groups.empty())
        {
            std::cout << "none (no rules apply to this user)\n";
            return;
        }
        for (const auto &group : groups)
        {
            std::cout << group << "\n";
        }
        return;
    }

    // all users (usernames only)
    if (users.empty())
    {
        std::cout << "No users in the database.\n";
        return;
    }
    std::cout << "--- Users ---\n";
    for (const auto &u : users)
    {
        std::cout << u << "\n";
    }
}

int main(int argc, char *argv[])
{
    Options opts;
    try
    {
        opts = parse_args(argc, argv);
    }
    catch (const std::exception &exc)
    {
        std::cerr << exc.what() << "\n";
        return -1;
    }

    try
    {
        if (opts.command == "add")
        {
            cmd_add(opts);
        }
        else if (opts.command == "edit")
        {
            cmd_edit(opts);
        }
        else if (opts.command == "remove")
        {
            cmd_remove(opts);
        }
        else
        {
            cmd_show(opts);
        }
    }
    catch (const std::exception &exc)
    {
        std::cerr << exc.what() << "\n";
        OPENSSL_cleanse(opts.password.data(), opts.password.size());
        return -2;
    }

    // wipe the local copy of the password (the argv copy is not
    // removable, it belongs to the C runtime)
    OPENSSL_cleanse(opts.password.data(), opts.password.size());

    return 0;
}
