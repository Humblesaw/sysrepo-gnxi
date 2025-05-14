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

#define BOOST_LOG_USE_NATIVE_SYSLOG

#include <iostream>
#include <filesystem>
#include <fstream>

#include <sysrepo-cpp/Connection.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/sinks.hpp>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include <libyang/libyang.h>
#include <sysrepo.h>
#include <sys/file.h>

#include "log.h"

using namespace logging::sinks;

extern "C" void signal_handler(int signum);

static const char* display_data_log_env = "GNMI_DISPLAY_DATA_LOG";
static bool display_data_log = true;
static void _boost_set_log_level(int lvl)
{
  logging::trivial::severity_level l;

  switch (lvl) {
    case 0: //no log
      l = logging::trivial::fatal;
      break;
    case 1: //only error message
      l = logging::trivial::error;
      break;
    case 2: //log error and warning
      l = logging::trivial::warning;
      break;
    case 3: //log error, warning, and informational
      l = logging::trivial::info;
      break;
    case 4: //log error, warning, informational and debug
      l = logging::trivial::debug;
      break;
    default:
      std::cerr << "Unused log level" << std::endl;
      exit(1);
  }

  logging::core::get()->set_filter(
    logging::trivial::severity >= l
  );
}

static void sysrepo_log_cb(sr_log_level_t level, const char *message)
{
  switch (level) {
    case SR_LL_ERR:
      BOOST_LOG_TRIVIAL(error) << "[" << gettid() << "] " << message;
      break;
    case SR_LL_WRN:
      BOOST_LOG_TRIVIAL(warning) << "[" << gettid() << "] " << message;
      break;
    case SR_LL_INF:
      /* Log at info at debug level to avoid sending sysrepo logs to the OSS. */
    case SR_LL_DBG:
      BOOST_LOG_TRIVIAL(debug) << "[" << gettid() << "] " << message;
      break;
    default:
      break;
  }
}

static void libyang_log_cb(LY_LOG_LEVEL level, const char *message,
        const char *data_path, const char *schema_path, uint64_t _line)
{
  (void)_line;
  std::string path_message = "";
  if (data_path || schema_path) {
    path_message = " (path: " + std::string(data_path ? data_path : schema_path) + ")";
  }

  switch (level) {
    case LY_LLERR:
      BOOST_LOG_TRIVIAL(error) << message << path_message;
      break;
    case LY_LLWRN:
      BOOST_LOG_TRIVIAL(warning) << message << path_message;
      break;
    case LY_LLVRB:
      /* Log at info at debug level to avoid sending libyang logs to the OSS. */
    case LY_LLDBG:
      BOOST_LOG_TRIVIAL(debug) << message << path_message;
      break;
    default:
      break;
  }
}

Log::Log(int lvl)
{
  setLevel(lvl);
}

void Log::setSyslogBackend()
{
  boost::shared_ptr<logging::core> core = logging::core::get();

  boost::shared_ptr<syslog_backend> backend(new syslog_backend(
      logging::keywords::facility = syslog::user,
      logging::keywords::use_impl = syslog::native
  ));

  auto severity_mapping = syslog::custom_severity_mapping<logging::trivial::severity_level>("Severity");
  severity_mapping[logging::trivial::error] = syslog::error;
  severity_mapping[logging::trivial::warning] = syslog::warning;
  severity_mapping[logging::trivial::info] = syslog::info;
  severity_mapping[logging::trivial::debug] = syslog::debug;
  backend->set_severity_mapper(severity_mapping);

  core->add_sink(boost::make_shared<synchronous_sink<syslog_backend>>(backend));
}

void Log::setLevel(int lvl)
{
  _boost_set_log_level(lvl);

  //Libyang log level should be ERROR only
  ly_log_level(LY_LLERR);
  sr_log_set_cb(sysrepo_log_cb);
  ly_set_log_clb(libyang_log_cb);
}

void get_log_env(void)
{
  const char* var = std::getenv(display_data_log_env);

  if (var) {
    std::string value(var);
    if (value == "Y" || value == "YES" || value == "y" || value == "yes") {
      display_data_log = true;
    } else if (value == "N" || value == "NO" || value == "n" || value == "no") {
      display_data_log = false;
    } else {
      BOOST_LOG_TRIVIAL(warning) << "Unrecognized value for " << display_data_log_env << ":" << value;
    }
  }
  BOOST_LOG_TRIVIAL(debug) << "Logging of GNMI data is " << (display_data_log ? "ENABLED" : "DISABLED");
}

const char* obfs_data(std::string &data)
{
  if (display_data_log) {
    return data.c_str();
  } else {
    return "&*%#";
  }
}

static void trigger_logrotate(void)
{
  auto wstatus = system(GNMI_LOGROTATE_SCRIPT " " GNMI_LOG_DIR);

  // During execution of the command, SIGCHLD will be blocked, and SIGINT and SIGQUIT will be ignored,
  // in the process that calls system().
  // (These signals will be handled according to their defaults inside the child process that executes command.)
  //
  // So, check if logrotate was signalled, and pass it along to our own handler.

  if (WIFSIGNALED(wstatus)) {
    auto signal = WTERMSIG(wstatus);
    BOOST_LOG_TRIVIAL(info) << "log-rotate was signalled " << strsignal(signal);
    if (signal == SIGTERM || signal == SIGINT) {
      // pass the signal to the handler thread so we can terminate as well
      signal_handler(signal);
    }
  } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus)) {
    BOOST_LOG_TRIVIAL(error) << "log-rotate failed with " << WEXITSTATUS(wstatus);
  }
}

void log_to_file(const std::string data, std::string metadata, const uint64_t id)
{
  using namespace std::chrono;
  const size_t MAX_FILE_SIZE = 500 * 1024;

  static std::atomic<size_t> size;

  if (!display_data_log) {
    // Not writing any data to honor configured obfuscation
    BOOST_LOG_TRIVIAL(debug) << metadata << " data obfuscated";
    return;
  }

  if (!id) {
    // There is no log id, so keep it in the journal
    BOOST_LOG_TRIVIAL(debug) << metadata << ": " << data;
    return;
  }

  {
    auto now = boost::posix_time::microsec_clock::local_time();
    auto timestamp = boost::posix_time::to_iso_string(now);

    auto filename = std::string(GNMI_LOG_DIR) + "/raw/" + timestamp + "-pid." + std::to_string(getpid())
                      + "-transaction." + std::to_string(id);

    auto tmpfile = filename + ".tmp";

    auto fp = fopen(tmpfile.c_str(), "w");
    if (!fp) {
      throw std::runtime_error("Failed to open tmpfile " + tmpfile);
    }

    if (flock(fileno(fp), LOCK_NB | LOCK_EX) == -1) {
      fclose(fp);
      throw std::runtime_error("Failed to acquire flock on " + tmpfile);
    }

    auto livefile = filename + ".live";

    std::filesystem::rename(tmpfile, livefile);

    timestamp = boost::posix_time::to_simple_string(now);

    // JSON doesn't like unescaped double quotes - escape them
    boost::algorithm::replace_all(metadata, "\"", "\\\"");

    // quote data if it is not already JSON
    auto json_data = data;
    switch (data.front()) {
      case '{':
      case '"':
      case '[':
        // looks like valid JSON data.
        break;
      default:
        json_data = "\"" + data + "\"";
        break;
    }

    auto bytes_written = fprintf(fp, "{\"timestamp\": \"%s\", \"metadata\":\"%s\", \"data\": %s}",
            timestamp.c_str(), metadata.c_str(), json_data.c_str());

    std::filesystem::rename(livefile, filename + ".log");

    // unlock the flock by closing file pointer
    fclose(fp);

    size.fetch_add(bytes_written);
  }

  auto expected = size.load();

  if ((expected > MAX_FILE_SIZE) && size.compare_exchange_weak(expected, 0)) {
    std::thread(trigger_logrotate).detach();
  }
}

