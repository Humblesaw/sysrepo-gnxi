/**
 * @file encode.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Encoding handler header
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

#include <libyang-cpp/DataNode.hpp>
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>

/**
 * @brief Helper class to reset the session datastore on going out of scope.
 *
 */
class SessionDsSwitcher
{
  public:
    SessionDsSwitcher(sysrepo::Session sess, sysrepo::Datastore ds) : sr_sess_(sess)
    {
        orig_ds_ = sr_sess_.activeDatastore();
        sr_sess_.switchDatastore(ds);
    }

    ~SessionDsSwitcher() { sr_sess_.switchDatastore(orig_ds_); }

  private:
    sysrepo::Session sr_sess_;
    sysrepo::Datastore orig_ds_;
};

/**
 * @brief Purpose for the encode/decode.
 *
 */
enum class EncodePurpose
{
    SET,
    RPC,
};

/**
 * @brief Factory to instantiate encodings.
 *
 * Encoding can be {JSON, Bytes, Proto, ASCII, JSON_IETF}.
 */
class Encode
{
  public:
    Encode(sysrepo::Session sess) : sr_sess_(sess) {}

    /**
     * @brief Supported encodings.
     *
     */
    enum Supported
    {
        JSON_IETF = 0,
    };

    /**
     * @brief Decode a gNMI TypedValue into a libyang data node.
     *
     * @param[in] xpath XPath of the data to decode.
     * @param[in] reqval Typed value to decode.
     * @param[in] purpose Purpose for the encode/decode.
     * @return grpc::Status::OK and the parsed data node, an error status and
     * std::nullopt otherwise.
     */
    std::tuple<grpc::Status, std::optional<libyang::DataNode>> decode(
        const std::string &xpath, const gnmi::TypedValue &reqval, EncodePurpose purpose);

    /**
     * @brief Encode a libyang data node into a gNMI TypedValue of the given
     * encoding.
     *
     * @param[in] encoding Encoding to use.
     * @param[in] node Data node to encode.
     * @param[out] val Typed value receiving the encoded data.
     * @return grpc::Status::OK on success, an error status otherwise.
     */
    grpc::Status encode(gnmi::Encoding encoding, libyang::DataNode node, gnmi::TypedValue *val);

    /**
     * @brief Parse a message encoded in JSON IETF and set fields in sysrepo.
     *
     * @param[in] xpath XPath of the data to decode.
     * @param[in] data Input data encoded in JSON.
     * @param[in] purpose Purpose for the encode/decode.
     * @return The parsed data node.
     */
    std::optional<libyang::DataNode> json_decode(
        const std::string &xpath, const std::string &data, EncodePurpose purpose);

    /**
     * @brief Encode a libyang data node into JSON form.
     *
     * @param[in] node Data node to encode.
     * @return The data node encoded in JSON.
     */
    std::string json_encode(libyang::DataNode node);

  private:
    sysrepo::Session sr_sess_;
};
