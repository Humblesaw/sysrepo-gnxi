/**
 * @file sysrepo.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Sysrepo helpers implementation
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

#include <cassert>

#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/Module.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <sysrepo.h>

#include "utils/sysrepo.h"

#define SR_YANG_MOD "sysrepo"

void UpdateTransaction::merge(std::optional<libyang::DataNode> &tree,
                              std::optional<libyang::DataNode> &node)
{
    if (node.has_value())
    {
        // the node must be top-level, mergeWithSiblings only works on sibling lists
        assert(!node->parent());
        if (tree.has_value())
        {
            tree.value().mergeWithSiblings(node.value());
        }
        else
        {
            tree = node;
        }
    }
}

void UpdateTransaction::push(std::optional<libyang::DataNode> &tree)
{
    if (tree.has_value())
    {
        final_tree = final_tree.has_value() ? final_tree->insertSibling(tree.value()) : tree;
    }
}

std::unordered_set<std::string> collect_xpath_mods(const libyang::Context &ly_ctx,
                                                   const char *xpath)
{
    std::unordered_set<std::string> ly_mod_set;
    std::optional<libyang::SchemaNode> parent = std::nullopt;
    libyang::Set<libyang::SchemaNode> set = ly_ctx.findXPath(std::string(xpath));

    for (const auto &schema_node : set)
    {
        /* get module of the first schema node */
        parent = schema_node;
        while (parent->parent() != std::nullopt)
        {
            parent = parent->parent();
        }
        auto ly_mod = parent->module();

        // skip the internal SR_YANG_MOD
        if (ly_mod.name() == SR_YANG_MOD)
            continue;

        /* add a module to the set */
        ly_mod_set.insert(ly_mod.name());
    }

    return ly_mod_set;
}

DataSubscribe::DataSubscribe(sysrepo::Session sess) : data_sess(sess) {}

/*
 * Similar to sysrepo::Subscribe::module_change_subscribe
 *
 * Works around:
 * 1. Subscribe::module_change_subscribe requiring a module.
 * 2. Subscribe::module_change_subscribe not providing user-supplied context to callback function.
 */
void DataSubscribe::data_change_subscribe(sysrepo::ModuleChangeCb cb, const char *xpath,
                                          uint32_t priority, sysrepo::SubscribeOptions opts)
{
    for (const auto &mod : collect_xpath_mods(data_sess.getContext(), xpath))
    {
        if (sub)
        {
            sub->onModuleChange(mod, cb, xpath, priority, opts);
        }
        else
        {
            sub = data_sess.onModuleChange(mod, cb, xpath, priority, opts);
        }
    }
}
