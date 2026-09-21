/**
 * @file log.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Logging header
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

#pragma once

#include <atomic>
#include <sstream>
#include <string>
#include <utility>

#define SLOG_FATAL(...) slog::log(0, __VA_ARGS__)
#define SLOG_ERROR(...) slog::log(1, __VA_ARGS__)
#define SLOG_WARN(...) slog::log(2, __VA_ARGS__)
#define SLOG_INFO(...) slog::log(3, __VA_ARGS__)
#define SLOG_DEBUG(...) slog::log(4, __VA_ARGS__)

/**
 * @namespace slog
 *
 * Levels and their meaning:
 *   FATAL (0) - the server fails (startup failures: sysrepo connection,
                 listening sockets, TLS material, ...).
 *   ERROR (1) - an internal/server-side failure (sysrepo or libyang
 *               internal errors, failed configuration application,
 *               encoding failures, unexpected exceptions) or an
 *               unexpected security event (authentication or
 *               authorization denial, missing or rejected credentials).
 *   WARN  (2) - a routine client-caused request failure the operator
 *               needs no action for (malformed paths or data, unsupported
 *               features, unmet commit preconditions). The operation
 *               as a whole continues.
 *   INFO  (3) - lifecycle and audit trail (listening endpoints,
 *               shutdown, every RPC with user/peer/outcome, applied
 *               configuration mutations and commit operations).
 *   DEBUG (4) - developer detail (xpaths, proto dumps, also any
 *               sysrepo/libyang internals, which deliver warnings and
 *               errors only). May contain operational data, never
 *               credentials or key material.
 *
 * Security policy: passwords, password hashes, private keys and other
 * credential material must never be logged at any level. Usernames and
 * peer addresses are considered audit information and are logged.
 *
 * Every log line has the form:
 *   2006-01-02T15:04:05.123Z [INFO ] [12345] [Set: user 'alice' from ipv4:...] message...
 * i.e. UTC timestamp with millisecond precision, a fixed-width level
 * label, the thread id and, while an RPC is being handled, the request
 * context identifying the RPC, user and peer (see RequestScope).
 */
namespace slog
{

inline std::atomic<int> lvl{3};

/**
 * @brief Set the server logging level:
 *          lvl 0 : fatal
 *          lvl 1 : error
 *          lvl 2 : warning
 *          lvl 3 : info
 *          lvl 4 : debug
 *
 * Propagates matching verbosity to the sysrepo and libyang loggers and
 * registers the log callbacks. Expected to be called once at startup,
 * before any gRPC worker thread exists.
 *
 * @param[in] lvl Logging level to set, 0-4.
 */
void set_level(int lvl);

/**
 * @brief Emit an already assembled message on a complete log line.
 * Use predefined macros instead!
 *
 * @param[in] lvl Level of the message.
 * @param[in] msg Assembled message.
 */
void emit(int lvl, const std::string &msg);

/**
 * @brief Attach a request context to all log lines of the current thread.
 *
 * gRPC handles one RPC per worker thread, so installing the scope
 * at service entry ties all log output of that RPC.
 */
class RequestScope
{
  public:
    explicit RequestScope(std::string ctx);
    ~RequestScope();
    RequestScope(const RequestScope &) = delete;
    RequestScope &operator=(const RequestScope &) = delete;
    RequestScope(RequestScope &&) = delete;
    RequestScope &operator=(RequestScope &&) = delete;
};

/**
 * @brief Main logging function. Use predefined macros instead!
 *
 * @tparam Args
 * @param lvl Level at which to log the arguments.
 * @param args Arguments to log.
 */
template <class... Args> void log(int lvl, Args &&...args)
{
    if (lvl <= slog::lvl.load(std::memory_order_relaxed))
    {
        std::ostringstream oss;
        // avoid expensive copy of c++ structures by forwarding
        (oss << ... << std::forward<Args>(args));
        emit(lvl, oss.str());
    }
}

} // namespace slog
