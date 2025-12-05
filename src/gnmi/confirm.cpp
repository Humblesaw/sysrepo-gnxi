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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "confirm.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <utils/log.h>
#include <utils/utils.h>

namespace impl
{

// Large timeout value to be used when there's nothing to timeout
#define LARGE_TIMEOUT_SECS (7 * 24 * 60 * 60)

// Implements gNMI Confirm RPC
grpc::Status Confirm::run(const gnmi::ConfirmRequest *request, gnmi::ConfirmResponse *response)
{
    (void)request;  // unused
    (void)response; // unused
    grpc::Status status;

    if (not conf_state_->get_wait_confirm())
    {
        // We are not expecting for a Confirm RPC
        std::string err_str = "Not expecting Confirm RPC";
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, err_str);
    }
    // Now make sure enough time has elapsed
    uint64_t earliest_confirm_time_nsecs = conf_state_->get_earliest_confirm_time_nsecs();
    uint64_t crnt_time_ns = get_time_nanosec();
    if (crnt_time_ns < earliest_confirm_time_nsecs)
    {
        std::string err_str = "Confirm RPC too soon by " +
                              std::to_string(earliest_confirm_time_nsecs - crnt_time_ns) + " nsecs";
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, err_str);
    }
    SLOG_DEBUG("Ignore-system-state: ", request->ignore_system_state());
    if (not request->ignore_system_state())
    {
        // We have to check system state
    }

    try
    {
        sr_sess_startup_.copyConfig(sysrepo::Datastore::Running);
    }
    catch (sysrepo::ErrorWithCode &e)
    {
        SLOG_ERROR("Copy from running config to startup config failed: ", e.what(),
                   ". Transaction-id:", conf_state_->read_set_transaction_id());
        return grpc::Status(grpc::StatusCode::ABORTED, e.what());
    }

    // The last succesful set transaction has been confirmed
    conf_state_->write_confirmed_transaction_id(conf_state_->read_set_transaction_id());

    // All good, clear state
    conf_state_->clr_wait_confirm();
    return grpc::Status::OK;
}

// Callback function for timer expiry
static void check_confirm_expiry(ConfirmState *conf_state)
{
    if (conf_state->get_wait_confirm())
    {
        // Restore config only if the wait was not stopped
        conf_state->restore_config();
    }
}

// Handles confirm timeout or failure by restoring config
void ConfirmState::restore_config()
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    SLOG_ERROR("Restoring config: no valid Confirm RPC received");
    if (cfg_snapshot_.has_value())
    {
        std::string cfg_snapshot_json =
            cfg_snapshot_->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Siblings)
                .value();
        // Restore config
        try
        {
            sr_sess_.replaceConfig(cfg_snapshot_.value());
            // Restore transcation-id
            write_set_transaction_id(read_confirmed_transaction_id());
        }
        catch (const std::exception &e)
        {
            // Yikes
            SLOG_ERROR(e.what());
        }
        // Not waiting for confirm anymore
        clr_wait_confirm_no_lock_();
    }
    else
    {
        SLOG_ERROR("No config snapshot to restore");
    }
}
// Loop to check confirm timeout
void ConfirmState::check_confirm_loop(ConfirmState *conf_state)
{
    SLOG_DEBUG("Commit confirm timer thread started");

    while (not conf_state->timer_thread_exit_)
    {
        std::unique_lock<std::mutex> lock(conf_state->wait_mutex_);

        // waiting for timeout, notification or thread exit
        std::cv_status timeout =
            conf_state->cv_.wait_for(lock, std::chrono::seconds(conf_state->get_timeout_secs()));

        // timeout
        if (timeout == std::cv_status::timeout)
        {
            check_confirm_expiry(conf_state);
        }
    }

    SLOG_DEBUG("Commit confirm timer thread exited");
}

ConfirmState *ConfirmState::singleton_ = nullptr;

sysrepo::Session ConfirmState::createSession(sysrepo::Connection conn)
{
    try
    {
        return conn.sessionStart();
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR("Connection to sysrepo failed ", exc.what());
        exit(1);
    }
    SLOG_DEBUG("Commit confirm timer thread exited");
}

// Constructor for ConfirmState
ConfirmState::ConfirmState(sysrepo::Connection conn) : sr_sess_(createSession(conn))
{
    reset_default_timeout_secs();
    reset_min_wait_conf_secs();
    timeout_secs_ = LARGE_TIMEOUT_SECS;
    wait_confirm_ = false;
    timer_thread_exit_ = false;
    timer_thread_ = std::thread(check_confirm_loop, this);
    singleton_ = this;
}

ConfirmState::~ConfirmState()
{
    timer_thread_exit_ = true;
    cv_.notify_one();
    timer_thread_.join();
    singleton_ = nullptr;
}

bool ConfirmState::get_wait_confirm()
{
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    return wait_confirm_;
}

// Takes config snapshot, resets timer etc
bool ConfirmState::set_wait_confirm(uint32_t timeout_secs, std::string &err_msg)
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    if (wait_confirm_)
    {
        // Already waiting
        err_msg = "Already waiting for Confirm RPC";
        SLOG_ERROR(err_msg);
        return false;
    }

    // Get snapshot of current config
    cfg_snapshot_ = sr_sess_.getData("/*");

    wait_confirm_ = true;

    set_timeout_secs(timeout_secs);
    reset_timers();

    return true;
}
void ConfirmState::reset_timers()
{
    // Earliest time at which Confirm is accepted is now + min wait time
    auto crnt_time = get_time_nanosec();
    earliest_confirm_time_nsecs_ =
        crnt_time + (static_cast<uint64_t>(min_wait_conf_secs_) * 1000000000ull);

    // Stop the confirm timer so that it gets restarted
    cv_.notify_one();
}
void ConfirmState::clr_wait_confirm()
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    clr_wait_confirm_no_lock_();
}
// Useful when caller already has lock
void ConfirmState::clr_wait_confirm_no_lock_()
{
    wait_confirm_ = false;
    cfg_snapshot_ = std::nullopt;
    timeout_secs_ = LARGE_TIMEOUT_SECS;
    cv_.notify_one();
}

uint32_t ConfirmState::get_timeout_secs()
{
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);

    return (timeout_secs_);
}
uint32_t ConfirmState::get_min_wait_conf_secs()
{
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);

    return (min_wait_conf_secs_);
}
void ConfirmState::reset_default_timeout_secs()
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);

    default_timeout_secs_ = 300;
}
void ConfirmState::reset_min_wait_conf_secs()
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);

    min_wait_conf_secs_ = 30;
}
void ConfirmState::set_timeout_secs(uint32_t timeout_secs)
{
    if ((timeout_secs == 0) or (timeout_secs < min_wait_conf_secs_))
    {
        // No value or too small value was provided, use default
        timeout_secs_ = default_timeout_secs_;
    }
    else
    {
        timeout_secs_ = timeout_secs;
    }
}
uint64_t ConfirmState::get_earliest_confirm_time_nsecs()
{
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);

    return earliest_confirm_time_nsecs_;
}
uint32_t ConfirmState::get_num_events_service_failures()
{
    return num_events_service_failures_;
}
uint64_t ConfirmState::read_set_transaction_id()
{
    return set_transaction_id_;
}
void ConfirmState::write_set_transaction_id(uint64_t id)
{
    SLOG_DEBUG("write_set_transaction_id():", id);
    set_transaction_id_ = id;
}
uint64_t ConfirmState::read_confirmed_transaction_id()
{
    return confirmed_transaction_id_;
}
void ConfirmState::write_confirmed_transaction_id(uint64_t id)
{
    SLOG_DEBUG("write_confirmed_transaction_id():", id);

    confirmed_transaction_id_ = id;
}

} // namespace impl
