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
    commit_username_ = username;
    rollback_secs_ = rollback_secs;
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
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
        }
        if (commit_id_ != commit_id)
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
        }
        if (commit_username_ != username)
        {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "only the original user can confirm this commit");
        }

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
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
        }
        if (commit_id_ != commit_id)
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
        }
        if (commit_username_ != username)
        {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "only the original user can cancel this commit");
        }

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
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not waiting for confirm");
    }
    if (commit_id_ != commit_id)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "commit id mismatch");
    }
    if (commit_username_ != username)
    {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "only the original user can set rollback duration for this commit");
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
            restore_config_no_lock_();
            break;
        }
    }

    SLOG_DEBUG("Commit confirm timer thread exited");
}

} // namespace impl
