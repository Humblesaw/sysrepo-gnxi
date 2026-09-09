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

#include <string>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include "log.h"

static void sysrepo_log_cb(sr_log_level_t level, const char *message)
{
    switch (level)
    {
    case SR_LL_ERR:
        SLOG_ERROR("[", gettid(), "] ", message);
        break;
    case SR_LL_WRN:
        SLOG_WARN("[", gettid(), "] ", message);
        break;
    case SR_LL_INF:
        /* Log at info at debug level to avoid sending sysrepo logs to the OSS. */
    case SR_LL_DBG:
        SLOG_DEBUG("[", gettid(), "] ", message);
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
        SLOG_ERROR(message, path_message);
        break;
    case LY_LLWRN:
        SLOG_WARN(message, path_message);
        break;
    case LY_LLVRB:
        /* Log at info at debug level to avoid sending libyang logs to the OSS. */
    case LY_LLDBG:
        SLOG_DEBUG(message, path_message);
        break;
    default:
        break;
    }
}

void slog::set_level(int lvl)
{
    slog::lvl = lvl;
    // libyang log level should be ERROR only
    ly_log_level(LY_LLERR);
    sr_log_set_cb(sysrepo_log_cb);
    ly_set_log_clb(libyang_log_cb);
}
