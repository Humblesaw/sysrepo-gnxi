/**
 * @file path.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief gNMI Path conversions implementation
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

#include "utils/path.h"

#include <optional>
#include <vector>

#include <libyang-cpp/Enum.hpp>
#include <libyang-cpp/SchemaNode.hpp>

namespace gnxi
{
namespace
{

/**
 * @brief Check whether the schema node carries data, i.e. can appear in the
 * data tree (RPCs, actions, notifications and the structural choice/case
 * nodes cannot).
 */
bool isDataNode(const libyang::SchemaNode &node)
{
    switch (node.nodeType())
    {
    case libyang::NodeType::Container:
    case libyang::NodeType::Leaf:
    case libyang::NodeType::Leaflist:
    case libyang::NodeType::List:
    case libyang::NodeType::AnyXML:
    case libyang::NodeType::AnyData:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Check whether the subtree of the schema node contains a (data or RPC)
 * node of the given name directly below the (possibly choice/case-obscured)
 * top level.
 */
bool subtreeHasTopNode(const libyang::SchemaNode &node, const std::string &name)
{
    for (const auto &child : node.immediateChildren())
    {
        if (isDataNode(child) && child.name() == name)
        {
            return true;
        }
        if (child.nodeType() == libyang::NodeType::Choice ||
            child.nodeType() == libyang::NodeType::Case)
        {
            if (subtreeHasTopNode(child, name))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Check whether the module defines a top-level node of the given name
 * (descends through top-level choices and cases, includes top-level RPCs,
 * nested actions are addressed through their parent data nodes).
 */
bool moduleHasTopNode(const libyang::Module &module, const std::string &name)
{
    for (const auto &child : module.immediateChildren())
    {
        if (isDataNode(child) && child.name() == name)
        {
            return true;
        }
        if (child.nodeType() == libyang::NodeType::Choice ||
            child.nodeType() == libyang::NodeType::Case)
        {
            if (subtreeHasTopNode(child, name))
            {
                return true;
            }
        }
    }
    for (const auto &rpc : module.actionRpcs())
    {
        if (rpc.name() == name)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check whether the module's data is addressable under the origin.
 */
bool originContainsModule(const std::string &origin, const libyang::Module &module)
{
    auto moduleOrigin = gnxi::moduleOrigin(module);
    return !moduleOrigin.has_value() || *moduleOrigin == origin;
}

/**
 * @brief Check that the module's data is addressable under the origin.
 *
 * @throws grpc::Status INVALID_ARGUMENT.
 */
void checkModuleOrigin(const std::string &origin, const libyang::Module &module)
{
    if (originContainsModule(origin, module))
    {
        return;
    }
    throw grpc::Status(grpc::INVALID_ARGUMENT,
                       "module \"" + module.name() + "\" is under origin \"" +
                           *gnxi::moduleOrigin(module) +
                           "\" and cannot be addressed under origin \"" + origin + "\"");
}

/**
 * @brief Resolve the first element name of a path into a module-qualified name.
 *
 * A qualified name (module:node) is verified against the implemented modules, a
 * bare name is looked up across all implemented modules under the origin and
 * must be unambiguous.
 *
 * @throws grpc::Status INVALID_ARGUMENT.
 */
std::string resolveTopName(const libyang::Context &ctx, const std::string &name,
                           const std::string &origin)
{
    auto separator = name.find(':');
    if (separator != std::string::npos)
    {
        auto moduleName = name.substr(0, separator);
        auto node = name.substr(separator + 1);
        auto module = ctx.getModuleImplemented(moduleName);
        if (!module.has_value())
        {
            throw grpc::Status(grpc::INVALID_ARGUMENT, "unknown module \"" + moduleName + "\"");
        }
        if (!moduleHasTopNode(*module, node))
        {
            throw grpc::Status(grpc::INVALID_ARGUMENT, "module \"" + moduleName +
                                                           "\" has no top-level node \"" + node +
                                                           "\"");
        }
        checkModuleOrigin(origin, *module);
        return name;
    }

    std::vector<std::string> candidates;
    for (const auto &module : ctx.modules())
    {
        if (!module.implemented() || !originContainsModule(origin, module))
        {
            continue;
        }
        if (moduleHasTopNode(module, name))
        {
            candidates.push_back(module.name() + ":" + name);
        }
    }
    if (candidates.empty())
    {
        throw grpc::Status(grpc::INVALID_ARGUMENT, "unknown path element \"" + name + "\"");
    }
    if (candidates.size() > 1)
    {
        std::string list;
        for (const auto &candidate : candidates)
        {
            if (!list.empty())
            {
                list += ", ";
            }
            list += candidate;
        }
        throw grpc::Status(grpc::INVALID_ARGUMENT,
                           "ambiguous path element \"" + name + "\", candidates: " + list);
    }
    return candidates.front();
}

/**
 * @brief Append the predicates of one path element.
 *
 * The delimiter is chosen as one not occurring in the value, since XPath 1.0
 * has no escape mechanism.
 *
 * @throws grpc::Status INVALID_ARGUMENT.
 */
void appendKeyPredicates(std::string &xpath, const gnmi::PathElem &elem)
{
    for (const auto &key : elem.key())
    {
        if (key.second == "*")
        {
            // wildcard: all instances, same as omitting the key
            continue;
        }
        char delimiter;
        if (key.second.find('\"') == std::string::npos)
        {
            delimiter = '\"';
        }
        else if (key.second.find('\'') == std::string::npos)
        {
            delimiter = '\'';
        }
        else
        {
            throw grpc::Status(grpc::INVALID_ARGUMENT,
                               "key \"" + key.first +
                                   "\" value contains both quote characters: " + key.second);
        }
        xpath += "[" + key.first + "=" + delimiter + key.second + delimiter + "]";
    }
}
} // namespace

std::string resolveOrigin(const gnmi::Path &prefix, const gnmi::Path &path)
{
    std::string origin;
    if (!prefix.origin().empty())
    {
        if (!path.origin().empty())
        {
            throw grpc::Status(
                grpc::INVALID_ARGUMENT,
                "origin must not be specified in both prefix and path (gNMI spec 2.7)");
        }
        origin = prefix.origin();
    }
    else
    {
        origin = path.origin();
    }
    if (origin.empty())
    {
        return DEFAULT_ORIGIN;
    }
    if (origin != DEFAULT_ORIGIN && origin != "rfc7951")
    {
        throw grpc::Status(grpc::UNIMPLEMENTED, "unsupported origin \"" + origin + "\"");
    }
    return origin;
}

std::string gnmiToXPath(const libyang::Context &ctx, const gnmi::Path &prefix,
                        const gnmi::Path &path)
{
    auto origin = resolveOrigin(prefix, path);

    std::string xpath;
    bool first = true;
    // set after a multi-level wildcard ("...") - its "//" already ends with the
    // separator of the element that follows
    bool separatorPending = false;

    for (const auto *part : {&prefix, &path})
    {
        for (const auto &elem : part->elem())
        {
            const auto &name = elem.name();
            if (name == "..")
            {
                throw grpc::Status(grpc::INVALID_ARGUMENT,
                                   "gNMI paths MUST be absolute, \"..\" is not allowed");
            }
            if (name.empty())
            {
                throw grpc::Status(grpc::INVALID_ARGUMENT, "path element with an empty name");
            }

            if (!separatorPending)
            {
                xpath += '/';
            }
            separatorPending = false;
            if (name == "*")
            {
                xpath += '*';
            }
            else if (name == "...")
            {
                // multi-level wildcard, like XPath "//"
                xpath += '/';
                separatorPending = true;
            }
            else
            {
                xpath += first ? resolveTopName(ctx, name, origin) : name;
                appendKeyPredicates(xpath, elem);
            }
            first = false;
        }
    }

    return xpath.empty() ? ROOT_XPATH : xpath;
}

std::optional<std::string> moduleOrigin(const libyang::Module &module)
{
    for (const auto &instance : module.extensionInstances())
    {
        auto definition = instance.definition();
        if (definition.module().name() == ORIGIN_EXTENSION_MODULE &&
            definition.name() == ORIGIN_EXTENSION_NAME)
        {
            return instance.argument();
        }
    }
    return std::nullopt;
}

bool dataNodeInOrigin(const libyang::DataNode &node, const std::string &origin)
{
    auto root = node;
    while (auto parent = root.parent())
    {
        root = *parent;
    }
    return originContainsModule(origin, root.schema().module());
}

PathFormat formatForOrigin(const std::string &origin)
{
    return origin == "rfc7951" ? PathFormat::Qualified : PathFormat::Bare;
}

void dataNodeToGnmiPath(const libyang::DataNode &node, PathFormat format, gnmi::Path &out)
{
    std::vector<libyang::DataNode> chain;
    for (std::optional<libyang::DataNode> current = node; current.has_value();
         current = current->parent())
    {
        chain.push_back(*current);
    }

    std::optional<std::string> previousModule;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        auto schema = it->schema();
        auto module = schema.module().name();
        auto elem = out.add_elem();
        if (format == PathFormat::Qualified &&
            (!previousModule.has_value() || *previousModule != module))
        {
            elem->set_name(module + ":" + schema.name());
        }
        else
        {
            elem->set_name(schema.name());
        }
        if (schema.nodeType() == libyang::NodeType::List)
        {
            for (const auto &key : schema.asList().keys())
            {
                if (auto value = it->findPath(key.name()))
                {
                    (*elem->mutable_key())[key.name()] = value->asTerm().valueStr();
                }
            }
        }
        previousModule = module;
    }
}
} // namespace gnxi
