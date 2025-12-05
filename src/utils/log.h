/*
 * Copyright 2020 Yohan Pipereau
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

#pragma once

#include <iostream>
#include <string>
#include <utility>

#define SLOG_FATAL(...) slog::log(0, "[FATAL] ", __VA_ARGS__)
#define SLOG_ERROR(...) slog::log(1, "[ERROR] ", __VA_ARGS__)
#define SLOG_WARN(...) slog::log(2, "[WARN] ", __VA_ARGS__)
#define SLOG_INFO(...) slog::log(3, "[INFO] ", __VA_ARGS__)
#define SLOG_DEBUG(...) slog::log(4, "[DEBUG] ", __VA_ARGS__)

namespace slog
{

inline int lvl = 4;

/**
 * @brief Set the server logging level:
 *          lvl 0 : fatal
 *          lvl 1 : error
 *          lvl 2 : warning
 *          lvl 3 : info
 *          lvl 4 : debug
 *
 * @param[in] lvl Logging level to set.
 */
void set_level(int lvl);

/**
 * @brief Use predefined macros instead! Main logging function.
 *
 * @tparam Args
 * @param lvl Level at which to log the arguments.
 * @param args Arguments to log.
 */
template <class... Args> void log(int lvl, Args &&...args)
{
    if (lvl <= slog::lvl)
    {
        // avoid expensive copy of c++ structures by forwarding
        (std::clog << ... << std::forward<Args>(args)) << "\n";
    }
}

/*
 * Used to get log environment variables
 */
void get_log_env(void);

/*
 * Returns the data as a char* if displaying of data in logs is enabled
 * else it "obfuscates" the data
 */
const char *obfs_data(std::string &data);

} // namespace slog
