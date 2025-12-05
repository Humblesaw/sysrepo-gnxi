/*
 * Copyright 2020 Yohan Pipereau
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

#include <string>
#include <sysrepo-cpp/Session.hpp>

#include <libyang/tree_data.h>
#include <utils/log.h>
#include <utils/sysrepo.h>

#include "encode.h"

/*****************
 * CRUD - UPDATE *
 *****************/

std::string stripJSONObjectValue(const std::string &object)
{
    std::string result = object;
    bool name_begin = false;
    int index_start = 0;

    /* not a valid JSON object */
    if (object.front() != '{' || object.back() != '}')
    {
        SLOG_ERROR("Unexpected input: JSON object does not have { or }");
        throw;
    }

    /* strip the JSON object brackets */
    result = object.substr(1, object.size() - 2);

    /* go through the JSON object and find the delimiter ':' between name and value */
    for (size_t i = 0; i < result.length(); ++i)
    {
        switch (result.at(i))
        {
        case '"':
            /* beginning or ending of a name */
            name_begin = !name_begin;
            break;
        case ':':
            /* skip all ':' inside of the name */
            if (!name_begin)
            {
                /* index of ':' between name and value */
                index_start = i;
            }
            break;
        }

        /* we have found the ':' */
        if (index_start)
        {
            break;
        }
    }

    /* not a valid JSON object */
    if (!index_start)
    {
        SLOG_ERROR("Unexpected input: JSON object does not have a :");
        throw;
    }

    return result.substr(index_start + 1);
}

/*
 * Parse a message encoded in JSON IETF and set fields in sysrepo.
 * @param data Input data encoded in JSON
 */
std::optional<libyang::DataNode> Encode::json_decode(std::string xpath, std::string data,
                                                     EncodePurpose purpose)
{
    /* get request */
    // TODO "/*" is a bad sign that this is a set/get request for all data, rewrite it
    if (xpath.compare("/*") == 0)
    {
        try
        {
            auto ctx = sr_sess.getContext();
            return ctx.parseData(data, libyang::DataFormat::JSON,
                                 libyang::ParseOptions::ParseOnly | libyang::ParseOptions::Strict,
                                 std::nullopt);
        }
        catch (const std::exception &exc)
        {
            SLOG_ERROR("Failed to parse data:", slog::obfs_data(data), ". Exception: ", exc.what());
            // Don't leave the error lying around on the context otherwise sysrepo may pick it up on
            // an unrelated operation
            auto ctx = sr_sess.getContext();
            const_cast<libyang::Context *>(&ctx)->cleanAllErrors();
            throw std::invalid_argument(exc.what());
        }
    }

    // Create a node tree according to the xpath and data value.
    try
    {
        auto ctx = sr_sess.getContext();
        std::optional<libyang::DataNode> node = std::nullopt;
        switch (purpose)
        {
        case EncodePurpose::Set:
            return ctx.parseValueFragment(
                xpath, data, libyang::DataFormat::JSON, std::nullopt,
                libyang::ParseOptions::JsonNull | libyang::ParseOptions::Strict, std::nullopt);
        case EncodePurpose::Rpc:
            // If xpath is not a path, this throws error (one node expected)
            node = ctx.newPath2(xpath).createdNode;
            if (node.has_value())
            {
                node.value().parseOp(data.c_str(), libyang::DataFormat::JSON,
                                     libyang::OperationType::RpcYang);
            }
            else
            {
                throw std::runtime_error("The node for the XPath has not been created.");
            }
            return node;
        default:
            throw std::runtime_error("Unsupported encode purpose.");
        }
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR("Failed to parse data. xpath: ", xpath, ", data:", slog::obfs_data(data),
                   ". Exception: ", exc.what());
        // Don't leave the error lying around on the context otherwise sysrepo may pick it up on
        // an unrelated operation
        auto ctx = sr_sess.getContext();
        const_cast<libyang::Context *>(&ctx)->cleanAllErrors();
        throw;
    }

    return std::nullopt;
}

/***************
 * CRUD - READ *
 ***************/

/* Encode a libyang data node into JSON form */
std::string Encode::json_encode(libyang::DataNode node)
{
    std::string data;

    if (node.schema().nodeType() == libyang::NodeType::Leaf)
    {
        data = stripJSONObjectValue(
            node.printStr(libyang::DataFormat::JSON,
                          libyang::PrintFlags::Shrink | libyang::PrintFlags::JsonNoNestedPrefix)
                .value());
    }
    else
    {
        // In case the node has no children
        data = "{}";
        // The xpath will have found the containing node, but we want to dump its children
        // according to gNMI rules
        if (node.child().has_value())
        {
            for (auto it : node.child()->childrenDfs())
            {
                data = it.printStr(libyang::DataFormat::JSON,
                                   libyang::PrintFlags::Siblings | libyang::PrintFlags::Shrink |
                                       libyang::PrintFlags::JsonNoNestedPrefix)
                           .value();
                break;
            }
        }
    }

    return data;
}
