/**
 * @file log.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Logging implementation
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
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <unistd.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include "log.h"

static thread_local std::string request_ctx;

slog::RequestScope::RequestScope(std::string ctx)
{
    request_ctx = std::move(ctx);
}

slog::RequestScope::~RequestScope()
{
    request_ctx.clear();
}

static const char *level_label(int lvl)
{
    switch (lvl)
    {
    case 0:
        return "[FATAL]";
    case 1:
        return "[ERROR]";
    case 2:
        return "[WARN ]";
    case 3:
        return "[INFO ]";
    default:
        return "[DEBUG]";
    }
}

void slog::emit(int lvl, const std::string &msg)
{
    // sysrepo/libyang callbacks may fire from any thread, so the whole
    // line is written under a mutex to keep concurrent output intact
    static std::mutex mtx;

    // split into the whole-second part (for put_time/strftime semantics,
    // which have no sub-second fields) and the sub-second millisecond part
    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    // gmtime_r fills the caller-provided tm (UTC) and is thread-safe
    gmtime_r(&tt, &tm);

    std::lock_guard<std::mutex> lock(mtx);
    // UTC ISO-8601 with milliseconds ("2026-09-22T08:57:39.803Z")
    std::cerr << std::put_time(&tm, "%FT%T") << '.' << std::setw(3) << std::setfill('0')
              << ms.count() << "Z " << level_label(lvl) << " [" << gettid() << "] ";
    if (!request_ctx.empty())
    {
        std::cerr << "[" << request_ctx << "] ";
    }
    std::cerr << msg << "\n";
}

static void sysrepo_log_cb(sr_log_level_t level, const char *message)
{
    switch (level)
    {
    case SR_LL_ERR:
    case SR_LL_WRN:
        SLOG_DEBUG("sysrepo: ", message);
        break;
    default:
        break;
    }
}

static void libyang_log_cb(LY_LOG_LEVEL level, const char *message, const char *data_path,
                           const char *schema_path, uint64_t _line)
{
    (void)_line;
    std::string path_message = "";
    if (data_path || schema_path)
    {
        path_message = " (path: " + std::string(data_path ? data_path : schema_path) + ")";
    }

    switch (level)
    {
    case LY_LLERR:
    case LY_LLWRN:
        SLOG_DEBUG("libyang: ", message, path_message);
        break;
    default:
        break;
    }
}

void slog::set_level(int lvl)
{
    slog::lvl.store(lvl);

    ly_log_level(LY_LLWRN);
    sr_log_set_cb_level(SR_LL_WRN);
    sr_log_set_cb(sysrepo_log_cb);
    ly_set_log_clb(libyang_log_cb);
}
