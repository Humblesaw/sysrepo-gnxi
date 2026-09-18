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
#include <utility>
#include <vector>

#include <libyang-cpp/DataNode.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>

#include <openssl/crypto.h>

#include "security/hash.h"
#include "utils/utils.h"

const char *USAGE = R"(Usage:
  sysrepo-gnxi-users add --name NAME --password PASS [--hash ALGO]
                         [--permissions MOD:PERM[,MOD:PERM...]]
  sysrepo-gnxi-users edit --name NAME [--password PASS [--hash ALGO]]
                          [--permissions MOD:PERM[,MOD:PERM...]]
  sysrepo-gnxi-users remove --name NAME
  sysrepo-gnxi-users show [--name NAME]

Options:
  -n,--name NAME        username
  -P,--password PASS    password
  -s,--hash ALGO        hashing algorithm: md5, sha256 or sha512 (default)
  -p,--permissions ...  comma-separated MOD:PERM entries, PERM being
                        'rw' (read-write), 'ro' (read-only) or 'no'
                        (no access, removes the module from the ACL, no-op
                        for modules not present in the ACL);
                        MOD may be '*' for all modules currently installed
                        in sysrepo (with 'no' for all modules in the ACL)

The user database is stored in sysrepo under
/sysrepo-gnxi-users:users and is protected by filesystem permissions.
)";

/**
 * @brief A permission for a single module.
 *
 */
struct Permission
{
    std::string module, access; // "rw", "ro" or "no" (no access)
};

/**
 * @brief Options for user creation/edit/deletion in the user database.
 *
 */
struct Options
{
    std::string command, name, password, hash;
    std::vector<Permission> permissions;
};

/**
 * @brief A user account from the database with its ACL.
 *
 */
struct UserInfo
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> acl;
};

/**
 * @brief Split a string into a vector of modules.
 *
 * @param[in] list List of modules split by commas provided from the command line.
 * @return Vector of modules.
 */
std::vector<std::string> split_modules(const std::string &list)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size())
    {
        auto comma = list.find(',', pos);
        auto item = list.substr(pos, comma == std::string::npos ? comma : comma - pos);
        if (!item.empty())
        {
            out.push_back(item);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

/**
 * @brief Parse a comma-separated list of MODULE:PERMISSION entries.
 *
 * @param[in] list Permissions provided from the command line.
 * @return Parsed permissions in the order they were given.
 */
std::vector<Permission> parse_permissions(const std::string &list)
{
    std::vector<Permission> out;
    for (const auto &item : split_modules(list))
    {
        const auto colon = item.find(':');
        const auto access = colon == std::string::npos ? "" : item.substr(colon + 1);
        if (colon == std::string::npos || item.substr(0, colon).empty() ||
            (access != "rw" && access != "ro" && access != "no"))
        {
            throw std::runtime_error("Invalid permission entry: " + item);
        }
        out.push_back({item.substr(0, colon), access});
    }
    return out;
}

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

    static struct option long_options[] = {
        {"name", required_argument, 0, 'n'}, {"password", required_argument, 0, 'P'},
        {"hash", required_argument, 0, 's'}, {"permissions", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},       {0, 0, 0, 0}};

    optind = 2; // skip program name and command
    int c;
    while ((c = getopt_long(argc, argv, "n:P:s:p:h", long_options, nullptr)) != -1)
    {
        switch (c)
        {
        case 'n':
            opts.name = optarg;
            break;
        case 'P':
            opts.password = optarg;
            break;
        case 's':
            opts.hash = optarg;
            break;
        case 'p':
            opts.permissions = parse_permissions(optarg);
            break;
        case '?':
        case 'h':
            std::cout << USAGE;
            std::exit(0);
        default:
            std::cerr << USAGE;
            std::exit(-1);
        }
    }

    if (opts.command == "show")
    {
        // show accepts only an optional --name
        if (!opts.password.empty() || !opts.hash.empty() || !opts.permissions.empty())
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
    if (opts.command == "edit" && opts.password.empty() && opts.permissions.empty())
    {
        std::cerr << "Nothing to edit: provide --password and/or --permissions\n\n" << USAGE;
        std::exit(-1);
    }

    return opts;
}

/**
 * @brief Get all of the module names inside sysrepo, excluding private and
 * internal modules.
 *
 * @return Sysrepo module names.
 */
std::vector<std::string> sysrepo_modules()
{
    sysrepo::Connection conn;
    std::vector<std::string> result;
    for (const auto &mod : conn.sessionStart().getContext().modules())
    {
        if (mod.implemented() && mod.name() != "sysrepo" && !isPrivateModule(mod.name()))
        {
            result.push_back(mod.name());
        }
    }
    return result;
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
 * @brief Set permissions for the @name user.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] name Username to change permissions for.
 * @param[in] opts Command line options (read/write permissions).
 */
void set_acl(sysrepo::Session &sess, const std::string &name, const Options &opts)
{
    std::string user_path = "/sysrepo-gnxi-users:users/user[name='" + name + "']";

    auto add_acl = [&](const std::string &module, const char *access)
    { sess.setItem(user_path + "/acl[module='" + module + "']/access", access); };
    // apply the entries in the given order, the last one wins on conflicts
    for (const auto &perm : opts.permissions)
    {
        if (perm.access == "no")
        {
            if (perm.module == "*")
            {
                sess.deleteItem(user_path + "/acl");
            }
            else
            {
                sess.deleteItem(user_path + "/acl[module='" + perm.module + "']");
            }
        }
        else
        {
            for (const auto &m :
                 perm.module == "*" ? sysrepo_modules() : std::vector<std::string>{perm.module})
            {
                add_acl(m, perm.access.c_str());
            }
        }
    }
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
    set_acl(sess, opts.name, opts);
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' added to sysrepo\n";
}

/**
 * @brief Edit user's password and/or permissions in the database.
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
    if (!opts.password.empty())
    {
        sess.setItem(user_path + "/password", make_hash(opts.password, opts.hash));
    }
    set_acl(sess, opts.name, opts);
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' updated in sysrepo\n";
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
    sess.applyChanges();
    std::cout << "User '" << opts.name << "' removed from sysrepo\n";
}

/**
 * @brief Collect all users with their ACLs from sysrepo.
 *
 * @param[in] sess Sysrepo session.
 * @return Users with their ACLs, sorted by username and module name.
 */
std::vector<UserInfo> collect_users(sysrepo::Session &sess)
{
    std::vector<UserInfo> users;
    auto data = sess.getData("/sysrepo-gnxi-users:users");
    if (data.has_value())
    {
        for (auto user : data->findXPath("/sysrepo-gnxi-users:users/user"))
        {
            UserInfo u;
            u.name = user.findPath("name")->asTerm().valueStr();
            for (auto entry : user.findXPath("acl"))
            {
                u.acl.emplace_back(entry.findPath("module")->asTerm().valueStr(),
                                   entry.findPath("access")->asTerm().valueStr());
            }
            std::sort(u.acl.begin(), u.acl.end());
            users.push_back(std::move(u));
        }
    }
    std::sort(users.begin(), users.end(),
              [](const UserInfo &a, const UserInfo &b) { return a.name < b.name; });
    return users;
}

/**
 * @brief Show all users, or the module permissions of a specific user.
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
        auto it = std::find_if(users.begin(), users.end(),
                               [&](const UserInfo &u) { return u.name == opts.name; });
        if (it == users.end())
        {
            throw std::runtime_error("User '" + opts.name + "' not found in sysrepo");
        }
        std::cout << "--- " << it->name << " ---\n";
        if (it->acl.empty())
        {
            std::cout << "No module permissions.\n";
            return;
        }
        // align the permissions on the longest module name
        size_t width = 0;
        for (const auto &entry : it->acl)
        {
            width = std::max(width, entry.first.size());
        }
        for (const auto &entry : it->acl)
        {
            std::cout << entry.first << std::string(width - entry.first.size(), ' ') << " | "
                      << (entry.second == "rw" ? "read-write" : "read-only") << "\n";
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
        std::cout << u.name << "\n";
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
