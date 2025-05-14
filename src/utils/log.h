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

#ifndef _LOG_H
#define _LOG_H

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <syslog.h>

namespace logging = boost::log;

/*
 * Pick your severity
 * BOOST_LOG_TRIVIAL(trace) << "A trace severity message";
 * BOOST_LOG_TRIVIAL(debug) << "A debug severity message";
 * BOOST_LOG_TRIVIAL(info) << "An informational severity message";
 * BOOST_LOG_TRIVIAL(warning) << "A warning severity message";
 * BOOST_LOG_TRIVIAL(error) << "An error severity message";
 * BOOST_LOG_TRIVIAL(fatal) << "A fatal severity message";
*/
class Log {
  public:
    /*
     * lvl 0 : fatal
     * lvl 1 : error
     * lvl 2 : warning
     * lvl 3 : info
     * lvl 4 : debug
     */
    Log(int lvl = 4); // default to 'debug' log
    ~Log() {}

    void setLevel(int lvl);
    void setSyslogBackend();
};

/*
 * Used to get log environment variables
 */
void get_log_env(void);

/*
 * Returns the data as a char* if displaying of data in logs is enabled
 * else it "obfuscates" the data
 */
const char* obfs_data(std::string& data);

void log_to_file(const std::string data, std::string metadata, const uint64_t log_id);

#endif // _LOG_H
