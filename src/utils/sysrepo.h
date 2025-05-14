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

#ifndef _UTILS_SYSREPO_H
#define _UTILS_SYSREPO_H

#include <iostream>
#include <libyang/libyang.h>
#include <libyang-cpp/DataNode.hpp>
#include <libyang-cpp/Collection.hpp>
#include <sysrepo-cpp/Subscription.hpp>

class UpdateTransaction {
  public:
    /** Push a node and all its siblings into the transaction */
    void push(libyang::DataNode node)
    {
      for (auto n : node.siblings())
        push_one(n);
    }

    /** Push a node without its siblings into the transaction */
    void push_one(libyang::DataNode node)
    {
      auto dup = node.duplicate(libyang::DuplicationOptions::Recursive);
      first_node = first_node.has_value() ? first_node->insertSibling(dup) : dup;
    }

    std::optional<libyang::DataNode> first_node;
};

class DataSubscribe
{
public:
    DataSubscribe(sysrepo::Session sess);
    void data_change_subscribe(sysrepo::ModuleChangeCb cb, const char *xpath, uint32_t priority = 0, sysrepo::SubscribeOptions opts = sysrepo::SubscribeOptions::Default);

private:
    std::optional<sysrepo::Subscription> sub;
    /* The session is also available in the base class, but it is private so is duplicated here */
    sysrepo::Session data_sess;
};

#endif /* _UTILS_SYSREPO_H */
