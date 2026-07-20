/**
 * @file commit.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Commit confirmed extension implementation
 *
 * @copyright
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
#include <condition_variable>
#include <mutex>
#include <thread>

#include "commit.h"
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <utils/log.h>
#include <utils/utils.h>

namespace impl
{

Commit *Commit::singleton_ = nullptr;

Commit::Commit(sysrepo::Session sess) : sr_sess_(sess)
{
    timer_thread_exit_ = false;
    wait_confirm_ = false;
    timer_reset_ = false;
    rollback_secs_ = 0;
    timer_thread_ = std::thread(&Commit::check_confirm_loop_, this);
    singleton_ = this;
}

Commit::~Commit()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timer_thread_exit_ = true;
    }
    cv_.notify_one();
    timer_thread_.join();
    singleton_ = nullptr;
}

/**
 * @brief Check whether we are waiting for commit confirm.
 *
 * @return true Commit confirm is expected.
 * @return false No commit was issued. Commit confirm is not expected.
 */
bool Commit::get_wait_confirm()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return wait_confirm_;
}

/**
 * @brief Get the current rollback duration.
 *
 * @return Rollback duration in seconds.
 */
int64_t Commit::get_rollback_secs()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rollback_secs_;
}

/**
 * @brief Reset to the before-commit state.
 *
 */
void Commit::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clear_no_lock_();
}

/**
 * @brief Commit request: gnmi's CommitRequest. Checks and sets up the internal state variables.
 *
 * @param[in] commit_id Commit.id - must match the CommitRequest id.
 * @param[in] rollback_secs The number of seconds to set the timeout to.
 * @return grpc::Status::OK on success, failure otherwise.
 */
grpc::Status Commit::request_setup(const std::string &commit_id, int64_t rollback_secs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (wait_confirm_)
    {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "commit already in progress");
    }
    if (commit_id.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id required");
    }
    if (rollback_secs <= 0)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "rollback_duration must be greater than 0");
    }

    // get snapshot of the current config
    cfg_snapshot_ = sr_sess_.getData("/*");
    wait_confirm_ = true;
    commit_id_ = commit_id;
    rollback_secs_ = rollback_secs;
    return grpc::Status::OK;
}

/**
 * @brief Finishes the commit: gnmi's CommitRequest. Starts the rollback timer.
 *
 */
void Commit::request_finish()
{
    // notify the timer thread to start the rollback countdown
    cv_.notify_one();
}

/**
 * @brief Confirm commit: gnmi's CommitConfirm.
 *
 * @param[in] commit_id Commit.id - must match the CommitRequest id.
 * @return grpc::Status::OK on success, failure otherwise.
 */
grpc::Status Commit::confirm(const std::string &commit_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wait_confirm_)
    {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
    }
    if (commit_id_ != commit_id)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
    }

    // clear the internal state
    clear_no_lock_();
    return grpc::Status::OK;
}

/**
 * @brief Cancel commit: gnmi's CommitCancel.
 *
 * @param[in] commit_id Commit.id - must match the CommitRequest id.
 * @return grpc::Status::OK on success, failure otherwise.
 */
grpc::Status Commit::cancel(const std::string &commit_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wait_confirm_)
    {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
    }
    if (commit_id_ != commit_id)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
    }

    // per spec, cancel MUST rollback the configuration to the state prior to the
    // SetRequest that initiated the confirmed commit
    restore_config_no_lock_();
    return grpc::Status::OK;
}

/**
 * @brief Set rollback duration: gnmi's CommitSetRollbackDuration.
 *
 * @param[in] commit_id Commit.id - must match the CommitRequest id.
 * @param[in] rollback_secs The number of seconds to reset the timeout to.
 * @return grpc::Status::OK on success, failure otherwise.
 */
grpc::Status Commit::set_rollback_duration(const std::string &commit_id, int64_t rollback_secs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wait_confirm_)
    {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
    }
    if (commit_id_ != commit_id)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
    }
    if (rollback_secs <= 0)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "rollback_duration must be greater than 0");
    }

    timer_reset_ = true;
    rollback_secs_ = rollback_secs;

    // reset the confirm loop
    cv_.notify_one();
    return grpc::Status::OK;
}

/**
 * @brief Reset private values (to before-commit state). Caller handles locking.
 *
 */
void Commit::clear_no_lock_()
{
    cfg_snapshot_ = std::nullopt;
    wait_confirm_ = false;
    timer_reset_ = false;
    rollback_secs_ = 0;
    commit_id_.clear();

    // reset the confirm loop
    cv_.notify_one();
}

/**
 * @brief Handles commit rollback by restoring config. Caller handles locking.
 *
 */
void Commit::restore_config_no_lock_()
{
    SLOG_DEBUG("Restoring config");
    if (cfg_snapshot_.has_value())
    {
        // restore config
        try
        {
            sr_sess_.replaceConfig(cfg_snapshot_.value());
        }
        catch (const std::exception &e)
        {
            SLOG_ERROR(e.what());
        }
    }
    else
    {
        SLOG_ERROR("No config snapshot to restore");
    }

    // not waiting for confirm anymore
    clear_no_lock_();
}

/**
 * @brief Loop to check timeout for rollback.
 *
 */
void Commit::check_confirm_loop_()
{
    SLOG_DEBUG("Commit confirm timer thread started");

    std::unique_lock<std::mutex> lock(mutex_);
    while (not timer_thread_exit_)
    {
        // unconditionally block until notified
        // in case of spurious wakeup - we recheck below
        cv_.wait(lock);

        // wait for confirm, cancel, rollback timeout or shutdown
        while (wait_confirm_ and not timer_thread_exit_)
        {
            auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(rollback_secs_);

            // wait for confirm / cancel, timer reset, rollback timeout or server shutdown
            // in case of spurious wakeup - we recheck the predicate
            cv_.wait_until(lock, timeout, [this]
                           { return not wait_confirm_ || timer_reset_ || timer_thread_exit_; });

            // timer reset
            if (timer_reset_)
            {
                timer_reset_ = false;
                continue;
            }

            // timer expired
            if (wait_confirm_ and not timer_thread_exit_)
            {
                restore_config_no_lock_();
                break;
            }
        }
    }

    SLOG_DEBUG("Commit confirm timer thread exited");
}

} // namespace impl
