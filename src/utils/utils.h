/**
 * @file utils.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Utilities implementation
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
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

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <proto/gnmi.grpc.pb.h>
#include <stdexcept>
#include <string>

/* Get current time since epoch in nanosec */
inline uint64_t get_time_nanosec()
{
    std::chrono::nanoseconds ts;
    ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    return ts.count();
}

/**
 * @brief Get contents of a file.
 *
 * @param[in] path Path to the file.
 * @return File contents.
 */
inline std::string get_file_content(const std::filesystem::path &path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
    {
        throw std::runtime_error("Cannot open file: " + path.string());
    }

    auto size = ifs.tellg();
    if (size <= 0)
    {
        throw std::runtime_error("File is empty: " + path.string());
    }

    ifs.seekg(0);
    std::string content(static_cast<size_t>(size), '\0');
    if (!ifs.read(content.data(), size))
    {
        throw std::runtime_error("Failed to read file: " + path.string());
    }

    return content;
}

// We don't conform to the gNMI spec in that namespaces on paths are
// required on input and generated on output, so to signal that deviation
// and leave the door open to supporting a gNMI-compliant mode later and
// interoperating with standard gNMI clients we enforce the origin to be set
inline void gnmi_check_origin(const gnmi::Path &prefix, const gnmi::Path &path)
{
    // prefix and path origins can diverge! (not compliant)
    if (prefix.origin().size() > 0)
    {
        if (prefix.origin().compare("rfc7951"))
        {
            throw std::invalid_argument("prefix must contain origin of \"rfc7951\" rather than\"" +
                                        prefix.origin() + "\"");
        }
    }
    else if (path.origin().compare("rfc7951"))
    {
        throw std::invalid_argument("path must contain origin of \"rfc7951\" rather than \"" +
                                    path.origin() + "\"");
    }
}

/* Conversion methods between xpaths and gNMI paths */
inline std::string gnmi_to_xpath(const gnmi::Path &path)
{
    std::string str = "";

    // This form is most convenient for sysrepo get operations and sysrepo
    // set operations require special handling
    if (path.elem_size() <= 0)
        return "/*";

    // iterate over the list of PathElem of a gNMI path
    for (auto &node : path.elem())
    {
        str += "/";

        if (node.name().compare("..") == 0)
            throw std::invalid_argument("Relative paths not allowed");

        str += node.name();
        for (auto key : node.key())
        {
            // YANG 1.1 uses XPath 1.0 and it doesn't support escaping quotes:
            // >   	Literal	   ::=   	'"' [^"]* '"'
            // >                    | "'" [^']* "'"
            // Therefore, to avoid being able to inject potentially harmful user defined queries,
            // reject values with both double quotes and single quote.
            if ((key.second.find('\"') != std::string::npos) &&
                (key.second.find('\'') != std::string::npos))
                throw std::invalid_argument("Double quotes AND single quote in values not allowed");
            // Use " as delimiter unless it's present then use ' as delimiter
            auto delim = (key.second.find('\"') != std::string::npos) ? '\'' : '\"';
            str += "[" + key.first + "=" + delim + key.second + delim + "]";
        }
    }

    return str;
}

// Parse XPath-like string in gnmi::Path
// Assumes that the path is well-formed (i.e. hasn't come from the client)
inline void xpath_to_gnmi(std::string xpath, gnmi::Path &path)
{
    if (!xpath.compare("/"))
        return;

    path.set_origin("rfc7951");

    auto start = 0u;
    auto end = xpath.find_first_of('/', start);
    assert(end != std::string::npos);
    // Skip initial / - we don't want an empty path elem inserted for it
    start = end + 1;
    end = xpath.find_first_of('/', start);
    for (; true; start = end + 1, end = xpath.find_first_of('/', start))
    {
        auto elem = path.add_elem();
        auto key_start = xpath.find_first_of('[', start);
        auto key_close = std::string::npos;

        // Parse list key(s) if present
        if (key_start != std::string::npos && key_start < end)
        {
            size_t value_end = 0;
            elem->mutable_name()->assign(xpath.substr(start, key_start - start));
            for (; key_start < end; key_start = value_end + 2)
            {
                if (xpath[key_start] != '[')
                    break;

                // if 'end' fell on a slash '/' inside a key, move it on to next
                key_close = xpath.find_first_of(']', key_start);
                if (end < key_close)
                    end = xpath.find_first_of('/', key_close);

                auto key_end = xpath.find_first_of('=', key_start);
                // May be single or double quote character
                auto quote = xpath[key_end + 1];
                value_end = xpath.find_first_of(quote, key_end + 2);
                // +1 to skip over leading '['
                auto key = xpath.substr(key_start + 1, key_end - key_start - 1);
                // +2 to start to skip over = and '
                auto value = xpath.substr(key_end + 2, value_end - key_end - 2);
                (*elem->mutable_key())[key] = value;
            }
            // skip over the key & value
            start = value_end;
            end = xpath.find_first_of('/', start);
        }
        else
            elem->mutable_name()->assign(xpath.substr(start, end - start));

        if (end == std::string::npos)
            break;
    }
}

// Compare two gNMI paths, ignoring their targets
inline bool gnmi_path_equals(const gnmi::Path &path1, const gnmi::Path &path2)
{
    if (path1.origin() != path2.origin())
        return false;
    if (path1.elem_size() != path2.elem_size())
        return false;
    for (auto i = 0; i < path1.elem_size(); i++)
    {
        if (path1.elem(i).name() != path2.elem(i).name())
            return false;
        if (path1.elem(i).key_size() != path2.elem(i).key_size())
            return false;
        for (auto key_val1 : path1.elem(i).key())
        {
            bool found = false;
            for (auto key_val2 : path2.elem(i).key())
            {
                if (key_val2.first == key_val1.first && key_val2.second == key_val2.second)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
        }
    }
    return true;
}
