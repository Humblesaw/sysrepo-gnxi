/**
 * @file path.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNMI Path conversions header
 *
 * @copyright
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

#include <optional>
#include <string>

#include <grpc/status.h>
#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/DataNode.hpp>
#include <libyang-cpp/Module.hpp>
#include <proto/gnmi.grpc.pb.h>

namespace gnxi
{

// XPath that addresses the whole datastore (a gNMI root path, i.e. zero
// elements, maps to this)
inline constexpr const char *ROOT_XPATH = "/*";

// gNMI spec 2.7.1 - the default origin when unspecified
inline constexpr const char *DEFAULT_ORIGIN = "openconfig";

// YANG extension that binds a module to a single origin
inline constexpr const char *ORIGIN_EXTENSION_MODULE = "openconfig-extensions";
inline constexpr const char *ORIGIN_EXTENSION_NAME = "origin";

/**
 * @brief Validate the origin combination of a prefix and a path (gNMI spec 2.7)
 * and return the effective origin, normalized to DEFAULT_ORIGIN when
 * unspecified.
 *
 * A module annotated with oc-ext:origin is only addressable under that origin,
 * unannotated modules are addressable under every origin.
 *
 * @throws grpc::Status INVALID_ARGUMENT or UNIMPLEMENTED.
 */
std::string resolve_origin(const gnmi::Path &prefix, const gnmi::Path &path);

/**
 * @brief Convert a gNMI prefix + path pair into an XPath addressing the sysrepo
 * datastore.
 *
 * The element lists of the prefix and the path are concatenated and resolved as
 * one path, so a bare first element of the path continues the module context of
 * the prefix. The first element must resolve to a top-level schema node:
 * qualified names (module:node) are checked against the implemented modules,
 * bare names are looked up across all implemented modules under the effective
 * origin and must be unambiguous. Subsequent elements are appended verbatim,
 * the module context being resolved by libyang. Wildcards are supported ("*"
 * matches a single level, "..." any number of levels, a key value of "*"
 * selects all instances). A key value containing both single and double quotes
 * is rejected (XPath 1.0 predicates have no escaping).
 *
 * @throws grpc::Status INVALID_ARGUMENT or UNIMPLEMENTED.
 */
std::string gnmi_to_xpath(
    const libyang::Context &ctx, const gnmi::Path &prefix, const gnmi::Path &path);

/**
 * @brief Get the origin the module is bound to via oc-ext:origin, if any.
 *
 * A module without the annotation belongs to every origin.
 */
std::optional<std::string> module_origin(const libyang::Module &module);

/**
 * @brief Check whether the data node belongs to data addressable under the
 * given origin.
 *
 * Per gNMI spec 2.7.2 the origin is inherited from the root entity, so the
 * module of the top-level ancestor decides.
 */
bool data_node_in_origin(const libyang::DataNode &node, const std::string &origin);

// name format of emitted paths
enum class PathFormat
{
    BARE,      // openconfig convention: local element names
    QUALIFIED, // RFC 7951 convention: module-qualified names
};

/**
 * @brief Get the path name format matching the given (normalized) origin.
 *
 */
PathFormat format_for_origin(const std::string &origin);

/**
 * @brief Convert the path of a data node into a gNMI path.
 *
 * Keys of list instances are taken from the data, so arbitrary key values are
 * preserved exactly.
 */
void data_node_to_gnmi_path(const libyang::DataNode &node, PathFormat format, gnmi::Path &out);
} // namespace gnxi
