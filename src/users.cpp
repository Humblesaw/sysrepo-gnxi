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

#include <cstdlib>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>

#include "security/hash.h"
#include "utils/utils.h"

const char *USAGE = R"(Usage:
  sysrepo-gnxi-users add --name NAME --password PASS [--hash ALGO]
                        [--rm MOD[,MOD...]] [--ro MOD[,MOD...]] [--rw MOD[,MOD...]]
  sysrepo-gnxi-users edit --name NAME [--password PASS [--hash ALGO]]
                          [--rm MOD[,MOD...]] [--ro MOD[,MOD...]] [--rw MOD[,MOD...]]
  sysrepo-gnxi-users remove --name NAME

Options:
  -n,--name NAME      username ([a-zA-Z0-9_-]+)
  -p,--password PASS  password
  -s,--hash ALGO      digest algorithm: md5, sha1, sha224, sha256, sha384,
                      sha512, sha512-224, sha512-256, sha3-224, sha3-256,
                      sha3-384, sha3-512, blake2b512, blake2s256 or sha2
                      (= sha256), if omitted the password is stored in
                      plaintext
  -x,--rm MOD,...     comma-separated modules to remove from the ACL
                      (no-op in add mode since the user does not exist yet)
                      or '*' for all modules present in the ACL
  -r,--ro MOD,...     comma-separated modules with read-only access
                      or '*' for all modules currently installed in sysrepo
  -w,--rw MOD,...     comma-separated modules with read-write access
                      or '*' for all modules currently installed in sysrepo

The order of operations in add/edit mode is following: 1. --rm, 2. --ro, 3. --rw.

The user database is stored in sysrepo under
/sysrepo-gnxi-users:users and is protected by filesystem permissions.
)";

/**
 * @brief Options for user creation/edit/deletion in the user database.
 *
 */
struct Options
{
    std::string command, name, password, hash;
    std::vector<std::string> rm_mods, ro_mods, rw_mods;
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
    if (opts.command != "add" && opts.command != "edit" && opts.command != "remove")
    {
        std::cerr << "Unknown command: " << opts.command << "\n\n" << USAGE;
        std::exit(-1);
    }

    static struct option long_options[] = {
        {"name", required_argument, 0, 'n'}, {"password", required_argument, 0, 'p'},
        {"hash", required_argument, 0, 's'}, {"rm", required_argument, 0, 'x'},
        {"ro", required_argument, 0, 'r'},   {"rw", required_argument, 0, 'w'},
        {"help", no_argument, 0, 'h'},       {0, 0, 0, 0}};

    optind = 2; // skip program name and command
    int c;
    while ((c = getopt_long(argc, argv, "n:p:s:x:r:w:h", long_options, nullptr)) != -1)
    {
        switch (c)
        {
        case 'n':
            opts.name = optarg;
            break;
        case 'p':
            opts.password = optarg;
            break;
        case 's':
            opts.hash = optarg;
            break;
        case 'r':
            opts.ro_mods = split_modules(optarg);
            break;
        case 'w':
            opts.rw_mods = split_modules(optarg);
            break;
        case 'x':
            opts.rm_mods = split_modules(optarg);
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
    if (opts.command == "edit" && opts.password.empty() && opts.ro_mods.empty() &&
        opts.rw_mods.empty() && opts.rm_mods.empty())
    {
        std::cerr << "Nothing to edit: provide --password, --rm, --ro and/or --rw\n\n" << USAGE;
        std::exit(-1);
    }

    return opts;
}

/**
 * @brief Return hash for the password or the password.
 *
 * @param[in] opts Command line options (password and hash algorithm to use).
 * @return Password hash or the password (no hash algorithm chosen).
 */
std::string compute_stored_password(const Options &opts)
{
    if (opts.hash.empty())
    {
        std::cerr << "Warning: storing password for '" << opts.name << "' in plaintext\n";
    }
    return make_hash(opts.password, opts.hash);
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

    // 1. remove specified modules from the ACL
    if (!opts.rm_mods.empty())
    {
        if (opts.rm_mods[0] == "*")
        {
            sess.deleteItem(user_path + "/acl");
        }
        else
        {
            for (const auto &m : opts.rm_mods)
            {
                sess.deleteItem(user_path + "/acl[module='" + m + "']");
            }
        }
    }

    // 2. add read-only/read-write modules
    auto add_acl = [&](const std::string &module, const char *access)
    { sess.setItem(user_path + "/acl[module='" + module + "']/access", access); };
    if (!opts.ro_mods.empty())
    {
        for (const auto &m : opts.ro_mods[0] == "*" ? sysrepo_modules() : opts.ro_mods)
        {
            add_acl(m, "ro");
        }
    }
    if (!opts.rw_mods.empty())
    {
        for (const auto &m : opts.rw_mods[0] == "*" ? sysrepo_modules() : opts.rw_mods)
        {
            add_acl(m, "rw");
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
    sess.setItem(user_path + "/password", compute_stored_password(opts));
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
        sess.setItem(user_path + "/password", compute_stored_password(opts));
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
        else
        {
            cmd_remove(opts);
        }
    }
    catch (const std::exception &exc)
    {
        std::cerr << exc.what() << "\n";
        return -2;
    }

    return 0;
}
