/**
 * @file commit.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Commit confirmed extension header
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

#pragma once

#include <condition_variable>
#include <mutex>
#include <proto/gnmi.grpc.pb.h>
#include <thread>

#include <libyang-cpp/DataNode.hpp>
#include <sysrepo-cpp/Session.hpp>

namespace impl
{

class Commit
{
  public:
    // default rollback timeout for the Commit confirmed extension
    static constexpr int64_t default_rollback_secs = 600;

    Commit(sysrepo::Session sess);
    ~Commit();

    static Commit &get_singleton() { return *singleton_; }
    bool get_wait_confirm();
    int64_t get_rollback_secs();
    void clear();

    grpc::Status request_setup(const std::string &commit_id, int64_t rollback_secs);
    void request_finish();
    grpc::Status confirm(const std::string &commit_id);
    grpc::Status cancel(const std::string &commit_id);
    grpc::Status set_rollback_duration(const std::string &commit_id, int64_t rollback_secs);

  private:
    // singleton for tests
    static Commit *singleton_;
    // locking
    std::mutex mutex_;
    // timer
    bool timer_thread_exit_;
    std::thread timer_thread_;
    std::condition_variable cv_;
    // sysrepo session and snapshot
    sysrepo::Session sr_sess_;
    std::optional<libyang::DataNode> cfg_snapshot_;
    // whether we are waiting for a commit confirm
    bool wait_confirm_;
    // whether the timer was reset
    bool timer_reset_;
    // duration for rollback
    int64_t rollback_secs_;
    // the active commit id (from Commit.id)
    std::string commit_id_;

    void clear_no_lock_();
    void restore_config_no_lock_();
    void check_confirm_loop_();
};

} // namespace impl
