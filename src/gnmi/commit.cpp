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

Commit::Commit(sysrepo::Session sess) : sr_sess_(sess)
{
    // the timer thread is started on demand - only while a confirmed commit
    // is waiting for its confirm/cancel/rollback timeout
    timer_thread_exit_ = true;
    wait_confirm_ = false;
    timer_reset_ = false;
    rollback_secs_ = 0;
}

Commit::~Commit()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timer_thread_exit_ = true;
        // restore configuration on teardown
        if (wait_confirm_)
        {
            restore_config_no_lock_();
        }
    }
    cv_.notify_one();
    join_timer_thread_();
}

bool Commit::get_wait_confirm()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return wait_confirm_;
}

void Commit::clear()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_no_lock_();
    }
    join_timer_thread_();
}

grpc::Status Commit::request_setup(const std::string &commit_id, int64_t rollback_secs,
                                   const std::string &username)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (wait_confirm_)
    {
        SLOG_WARN("Commit request denied: commit '", commit_id_, "' already in progress");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "commit already in progress");
    }
    if (commit_id.empty())
    {
        SLOG_WARN("Commit request denied: commit id required");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id required");
    }
    if (rollback_secs <= 0)
    {
        SLOG_WARN("Commit request denied: rollback_duration must be greater than 0");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "rollback_duration must be greater than 0");
    }

    // get snapshot of the current config
    cfg_snapshot_ = sr_sess_.getData("/*");
    wait_confirm_ = true;
    commit_id_ = commit_id;
    commit_username_ = username;
    rollback_secs_ = rollback_secs;
    SLOG_INFO("Commit requested: id '", commit_id, "', user '",
              username.empty() ? "<insecure>" : username, "', rollback in ", rollback_secs, " s");
    return grpc::Status::OK;
}

void Commit::request_finish()
{
    // join a stale timer thread from a previous countdown (if any)
    join_timer_thread_();

    std::lock_guard<std::mutex> lock(mutex_);
    timer_thread_exit_ = false;
    // start the rollback countdown in a dedicated timer thread; the thread
    // exits as soon as the commit is confirmed/cancelled/rolled back
    timer_thread_ = std::thread(&Commit::check_confirm_loop_, this);
}

grpc::Status Commit::confirm(const std::string &commit_id, const std::string &username)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wait_confirm_)
        {
            SLOG_WARN("Commit confirm denied: not waiting for confirm");
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
        }
        if (commit_id_ != commit_id)
        {
            SLOG_WARN("Commit confirm denied: commit id mismatch");
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
        }
        if (commit_username_ != username)
        {
            SLOG_ERROR("Commit confirm denied: only the original user can confirm this commit");
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "only the original user can confirm this commit");
        }
        SLOG_INFO("Commit confirmed: id '", commit_id, "', user '",
                  username.empty() ? "<insecure>" : username, "'");

        // clear the internal state
        clear_no_lock_();
    }
    join_timer_thread_();
    return grpc::Status::OK;
}

grpc::Status Commit::cancel(const std::string &commit_id, const std::string &username)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wait_confirm_)
        {
            SLOG_WARN("Commit cancel denied: not waiting for confirm");
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
        }
        if (commit_id_ != commit_id)
        {
            SLOG_WARN("Commit cancel denied: commit id mismatch");
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
        }
        if (commit_username_ != username)
        {
            SLOG_ERROR("Commit cancel denied: only the original user can cancel this commit");
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "only the original user can cancel this commit");
        }
        SLOG_INFO("Commit cancelled: id '", commit_id, "', user '",
                  username.empty() ? "<insecure>" : username, "'");

        // per spec, cancel MUST rollback the configuration to the state prior to the
        // SetRequest that initiated the confirmed commit
        restore_config_no_lock_();
    }
    join_timer_thread_();
    return grpc::Status::OK;
}

grpc::Status Commit::set_rollback_duration(const std::string &commit_id, int64_t rollback_secs,
                                           const std::string &username)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wait_confirm_)
    {
        SLOG_WARN("Commit set rollback duration denied: not waiting for confirm");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
    }
    if (commit_id_ != commit_id)
    {
        SLOG_WARN("Commit set rollback duration denied: commit id mismatch");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
    }
    if (commit_username_ != username)
    {
        SLOG_ERROR(
            "Commit set rollback duration denied: only the original user can perform this action");
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "only the original user can set rollback duration for this commit");
    }
    if (rollback_secs <= 0)
    {
        SLOG_WARN("Commit set rollback duration denied: rollback_duration must be greater than 0");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "rollback_duration must be greater than 0");
    }

    timer_reset_ = true;
    rollback_secs_ = rollback_secs;
    SLOG_INFO("Commit rollback duration set: id '", commit_id, "', user '",
              username.empty() ? "<insecure>" : username, "', rollback in ", rollback_secs, " s");

    // reset the confirm loop
    cv_.notify_one();
    return grpc::Status::OK;
}

void Commit::clear_no_lock_()
{
    cfg_snapshot_ = std::nullopt;
    wait_confirm_ = false;
    timer_reset_ = false;
    rollback_secs_ = 0;
    commit_id_.clear();
    commit_username_.clear();

    // reset the confirm loop
    cv_.notify_one();
}

void Commit::restore_config_no_lock_()
{
    SLOG_INFO("Rolling back configuration of commit '", commit_id_, "' originally made by user '",
              commit_username_.empty() ? "<insecure>" : commit_username_, "'");
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

void Commit::join_timer_thread_()
{
    // claim the thread under the lock (so two callers cannot both join it),
    // but join without holding the lock - the thread needs it to finish
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (timer_thread_.joinable())
        {
            t = std::move(timer_thread_);
        }
    }
    if (t.joinable())
    {
        t.join();
    }
}

void Commit::check_confirm_loop_()
{
    SLOG_DEBUG("Commit confirm timer thread started");

    std::unique_lock<std::mutex> lock(mutex_);
    // wait for confirm, cancel, rollback timeout or shutdown
    // the thread only exists while a commit is pending
    while (wait_confirm_ && !timer_thread_exit_)
    {
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(rollback_secs_);

        // wait for confirm / cancel, timer reset, rollback timeout or server shutdown
        // in case of spurious wakeup - we recheck the predicate
        cv_.wait_until(lock, timeout,
                       [this] { return !wait_confirm_ || timer_reset_ || timer_thread_exit_; });

        // timer reset
        if (timer_reset_)
        {
            timer_reset_ = false;
            continue;
        }

        // timer expired
        if (wait_confirm_ && !timer_thread_exit_)
        {
            SLOG_INFO("Commit '", commit_id_, "' not confirmed in ", rollback_secs_,
                      " s, rolling back");
            restore_config_no_lock_();
            break;
        }
    }

    SLOG_DEBUG("Commit confirm timer thread exited");
}

} // namespace impl
