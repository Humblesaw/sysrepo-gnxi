/**
 * @file tls.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief TLS material loading implementation
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

#include "tls.h"

#include <stdexcept>
#include <vector>

#include <libyang-cpp/DataNode.hpp>

#include "crypto.h"
#include "utils/log.h"

/**
 * @brief Read a single configuration node by its absolute path. Throws on error.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] path Absolute path to the leaf.
 * @param[in] what Description of the leaf.
 * @return Leaf value.
 */
static std::string config_value(
    sysrepo::Session &sess, const std::string &path, const std::string &what)
{
    auto data = sess.getData(path);
    if (!data.has_value())
    {
        throw std::runtime_error(what + " not found");
    }

    auto node = data->findPath(path);
    if (!node.has_value())
    {
        throw std::runtime_error(what + " not found");
    }
    return node->asTerm().valueStr();
}

/**
 * @brief Read all configuration nodes matching an XPath.
 *
 * @param[in] sess Sysrepo session.
 * @param[in] xpath XPath to find.
 * @return Matching nodes.
 */
static std::vector<libyang::DataNode> config_values(
    sysrepo::Session &sess, const std::string &xpath)
{
    auto data = sess.getData(xpath);
    if (!data.has_value())
    {
        return {};
    }

    auto nodes = data->findXPath(xpath);
    std::vector<libyang::DataNode> values;
    for (auto node : nodes)
    {
        values.push_back(node);
    }
    return values;
}

TlsMaterial load_tls_material(sysrepo::Session &sess)
{
    TlsMaterial tls;

    // the referenced key/certificate/bag names fluctuate, read them first
    std::string key_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/asymmetric-key",
        "TLS server identity asymmetric-key reference");
    std::string cert_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/certificate",
        "TLS server identity certificate reference");
    std::string bag_name = config_value(sess,
        "/sysrepo-gnxi-server:server/tls/client-authentication/ca-certs/"
        "central-truststore-reference",
        "TLS client authentication CA-certs truststore reference");

    // private key of the server identity
    std::string key_path =
        "/ietf-keystore:keystore/asymmetric-keys/asymmetric-key[name='" + key_name + "']";
    tls.private_key_pem =
        pem_wrap_private_key(config_value(sess, key_path + "/cleartext-private-key",
                                 "cleartext-private-key of asymmetric-key '" + key_name + "'"),
            config_value(sess, key_path + "/private-key-format",
                "private-key-format of asymmetric-key '" + key_name + "'"));

    // server certificate chain - the referenced certificate first, then the
    // remaining certificates of the same asymmetric key (intermediates)
    tls.certificate_pem = pem_wrap_certificate(config_value(sess,
        key_path + "/certificates/certificate[name='" + cert_name + "']/cert-data",
        "certificate '" + cert_name + "' of asymmetric-key '" + key_name + "'"));
    for (auto node : config_values(
             sess, key_path + "/certificates/certificate[not(name='" + cert_name + "')]/cert-data"))
    {
        tls.certificate_pem += "\n" + pem_wrap_certificate(node.asTerm().valueStr());
    }

    // CA certificates for client verification
    for (auto node :
        config_values(sess, "/ietf-truststore:truststore/certificate-bags/certificate-bag[name='" +
                                bag_name + "']/certificate/cert-data"))
    {
        if (!tls.ca_certificate_pem.empty())
        {
            tls.ca_certificate_pem += "\n";
        }
        tls.ca_certificate_pem += pem_wrap_certificate(node.asTerm().valueStr());
    }
    if (tls.ca_certificate_pem.empty())
    {
        throw std::runtime_error(
            "certificate-bag '" + bag_name + "' not found or contains no certificates");
    }

    SLOG_INFO("TLS configuration loaded from sysrepo "
              "(key: ",
        key_name, ", cert: ", cert_name, ", CA bag: ", bag_name, ")");

    return tls;
}
