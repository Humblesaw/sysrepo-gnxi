/*
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

#ifndef _GNMI_CONFIRM_H
#define _GNMI_CONFIRM_H

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <proto/gnmi.grpc.pb.h>
#include <shared_mutex>
#include <sysrepo-cpp/Session.hpp>

using namespace gnmi;
using google::protobuf::RepeatedPtrField;
using grpc::Status;
using grpc::StatusCode;
using sysrepo::Connection;
using sysrepo::Session;

namespace impl {

// Class to manage the "state machine" for Confirm behaviour
class ConfirmState {
public:
  ConfirmState(sysrepo::Connection conn);
  ~ConfirmState();

  // Singleton for UT purposes only
  static ConfirmState &get_singleton() {
    return *singleton_;
  }
  // Returns true if we are currently waiting for a confirm
  bool get_wait_confirm();
  // Returns true on success, false on error (e.g. already waiting for a confirm)
  bool set_wait_confirm(uint32_t timeout_secs, std::string &err_msg);
  // Not waiting for confirm anymore
  void clr_wait_confirm();
  // Reset the timers for Confirm (min/max)
  void reset_timers();
  // Earliest time in ns, since epoch, to accept Confirm
  // Relevant when wait_confirm_ is true
  uint64_t get_earliest_confirm_time_nsecs();

  uint32_t get_timeout_secs();
  uint32_t get_min_wait_conf_secs();
  // Gets the stored snapshot for this counter
  uint32_t get_num_events_service_failures();
  // Handles Confirm timeout or failure by restoring the config
  void restore_config();
  // Updates/gets the transaction ids
  uint64_t read_set_transaction_id();
  void write_set_transaction_id(uint64_t id);
  uint64_t read_confirmed_transaction_id();
  void write_confirmed_transaction_id(uint64_t id);
  // Used for testing purposes
  void set_default_timeout_secs(uint32_t value);
  void reset_default_timeout_secs();
  void set_min_wait_conf_secs(uint32_t value);
  void reset_min_wait_conf_secs();
  void set_timeout_secs(uint32_t value);
private:
  // Whether we are waiting for a confirm RPC
  bool wait_confirm_;
  // For locking
  std::shared_timed_mutex mutex_;
  // For the timer
  std::thread timer_thread_;
  boost::asio::io_service io_;
  bool timer_thread_exit_;
  // Default timeout for Confirm (used when none specified)
  uint32_t default_timeout_secs_;
  // Timeout for Confirm
  uint32_t timeout_secs_;
  // Minimum time to wait before accepting a confirm
  uint32_t min_wait_conf_secs_;
  // Earliest time in ns, since epoch, to accept Confirm
  // Relevant when wait_confirm_ is true
  uint64_t earliest_confirm_time_nsecs_;
  // session to sysrepo
  sysrepo::Session sr_sess_;
  // Config snapshot
  std::optional<libyang::DataNode> cfg_snapshot_;
  // Snapshot of number of times services have failed
  uint32_t num_events_service_failures_;

  // On successful Set request, set_transaction_id is updated to request content
  // On successful Confirm request, confirmed_transaction_id is updated to
  // set_transaction_id.
  // On failed Set request, no transaction-id is updated
  // On Confirm timeout, when restoring config, set_transaction_id is reset
  // to confirmed_transaction_id
  uint64_t set_transaction_id_;
  uint64_t confirmed_transaction_id_;

private:
  static void check_confirm_loop(ConfirmState *conf_state);
  // Not waiting for confirm anymore
  void clr_wait_confirm_no_lock_();
  sysrepo::Session createSession(sysrepo::Connection conn);
  static ConfirmState *singleton_;
};

class Confirm {
public:
  Confirm(sysrepo::Session startup_sess, std::shared_ptr<ConfirmState> conf_state)
      : sr_sess_startup_(startup_sess), conf_state_(conf_state)
    {}
  ~Confirm() {}

  Status run(const ConfirmRequest *req, ConfirmResponse *response);

private:
  sysrepo::Session sr_sess_startup_;
  std::shared_ptr<ConfirmState> conf_state_;
};

} // namespace impl

#endif //_GNMI_CONFIRM_H
