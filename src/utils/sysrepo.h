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

#pragma once

#include <optional>

#include <libyang-cpp/DataNode.hpp>
#include <libyang/libyang.h>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>

class UpdateTransaction
{
  public:
    std::optional<libyang::DataNode> final_tree;

    /** Merge a top-level node into a tree */
    void merge(std::optional<libyang::DataNode> &tree, std::optional<libyang::DataNode> &node);

    /** Push a node and all its siblings into the final transaction tree */
    void push(std::optional<libyang::DataNode> &tree);
};

class DataSubscribe
{
  public:
    DataSubscribe(sysrepo::Session sess);
    void data_change_subscribe(sysrepo::ModuleChangeCb cb, const char *xpath, uint32_t priority = 0,
                               sysrepo::SubscribeOptions opts = sysrepo::SubscribeOptions::Default);

  private:
    std::optional<sysrepo::Subscription> sub;
    /* The session is also available in the base class, but it is private so is duplicated here */
    sysrepo::Session data_sess;
};
