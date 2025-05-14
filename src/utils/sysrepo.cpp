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

#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <sysrepo.h>
#include <string.h>

#include "utils/sysrepo.h"

#define SR_YANG_MOD "sysrepo"

static std::vector<libyang::Module>
collect_xpath_mods(libyang::Context ly_ctx, const char *xpath)
{
    std::vector<libyang::Module> mod_set;
    libyang::Module *ly_mod_ptr = nullptr;
    auto set = ly_ctx.findXpathAtoms(xpath, 0);

    for (auto node : set) {
        auto ly_mod = node.module();
        /* skip already-added modules */
        if (ly_mod_ptr && &ly_mod == ly_mod_ptr)
            continue;

        ly_mod_ptr = &ly_mod;

        /* skip import-only modules, and the internal SR_YANG_MOD */
        if (!ly_mod.implemented() || ly_mod.name() == SR_YANG_MOD)
            continue;

        mod_set.push_back(ly_mod);
    }

    return mod_set;
}

DataSubscribe::DataSubscribe(sysrepo::Session sess)
    : data_sess(sess)
{
}

/*
 * Similar to sysrepo::Subscribe::module_change_subscribe
 *
 * Works around:
 * 1. Subscribe::module_change_subscribe requiring a module.
 * 2. Subscribe::module_change_subscribe not providing user-supplied context to callback function.
 */
void DataSubscribe::data_change_subscribe(sysrepo::ModuleChangeCb cb, const char *xpath, uint32_t priority, sysrepo::SubscribeOptions opts)
{
    for (auto mod : collect_xpath_mods(data_sess.getContext(), xpath)) {
        if (sub) {
            sub->onModuleChange(
                std::string(mod.name()),
                cb,
                xpath,
                priority,
                opts);
        } else {
            sub = data_sess.onModuleChange(
                std::string(mod.name()),
                cb,
                xpath,
                priority,
                opts);
        }
    }
}

