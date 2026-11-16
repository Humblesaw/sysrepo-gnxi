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
#include <tuple>
#include <vector>

#include <libyang-cpp/DataNode.hpp>

#include "crypto.h"
#include "utils/log.h"

/**
 * @brief Read a single configuration node by its absolute path. Throws on error.
 *
 * @param[in] data Consistent configuration snapshot.
 * @param[in] path Absolute path to the leaf.
 * @param[in] what Description of the leaf.
 * @return Leaf value.
 */
static std::string config_value(
    const libyang::DataNode &data, const std::string &path, const std::string &what)
{
    auto node = data.findPath(path);
    if (!node.has_value())
    {
        throw std::runtime_error(what + " not found");
    }
    return node->asTerm().valueStr();
}

/**
 * @brief Read all configuration nodes matching an XPath.
 *
 * @param[in] data Consistent configuration snapshot.
 * @param[in] xpath XPath to find.
 * @return Matching nodes.
 */
static std::vector<libyang::DataNode> config_values(
    const libyang::DataNode &data, const std::string &xpath)
{
    auto nodes = data.findXPath(xpath);
    std::vector<libyang::DataNode> values;
    for (auto node : nodes)
    {
        values.push_back(node);
    }
    return values;
}

/**
 * @brief Find a named configuration list entry and return its properly quoted path.
 *
 * @param[in] data Consistent configuration snapshot.
 * @param[in] path Absolute path of the list entries.
 * @param[in] name Value of the entry's name key.
 * @param[in] what Description of the entry.
 * @return Data path of the entry.
 */
static std::string config_named_path(const libyang::DataNode &data, const std::string &path,
    const std::string &name, const std::string &what)
{
    for (const auto &node : config_values(data, path))
    {
        if (node.findPath("name")->asTerm().valueStr() == name)
        {
            return node.path();
        }
    }
    throw std::runtime_error(what + " '" + name + "' not found");
}

/**
 * @brief Reject a TLS subtree when it contains actual settings.
 *
 * Only configured terminals are rejected, implicit containers without settings are ignored.
 *
 * @param[in] data Consistent configuration snapshot.
 * @param[in] path Absolute subtree path.
 */
static void reject_tls_settings(const libyang::DataNode &data, const std::string &path)
{
    for (const auto &node : config_values(data, path + "//."))
    {
        if (node.isTerm())
        {
            throw std::runtime_error("unsupported TLS configuration: " + node.schema().path());
        }
    }
}

/**
 * @brief Check that the TLS configuration uses only the supported form.
 *
 * The supported schema forms are a central-keystore X.509 server identity
 * and client verification through a central-truststore CA bundle. Optional
 * settings that gRPC cannot honor are rejected instead of being ignored.
 *
 * @param[in] sess Sysrepo session used to inspect implemented features.
 * @param[in] data Consistent configuration snapshot.
 */
static void check_unsupported_tls_config(sysrepo::Session &sess, const libyang::DataNode &data)
{
    const std::string tls = "/sysrepo-gnxi-server:server/tls";

    if (config_values(data, tls + "/server-identity/certificate/"
                                  "central-keystore-reference/asymmetric-key")
            .empty())
    {
        throw std::runtime_error("TLS server identity must be an X.509 certificate referenced from "
                                 "the central keystore");
    }
    if (config_values(data, tls + "/client-authentication/ca-certs/"
                                  "central-truststore-reference")
            .empty())
    {
        throw std::runtime_error(
            "TLS client authentication with a central truststore CA bundle is required");
    }

    // querying a disabled feature's data path fails with NotFound, so skip it
    // without needing any exception handling
    static constexpr std::tuple<const char *, const char *, const char *> UNSUPPORTED[] = {
        {"ietf-tls-server", "client-auth-x509-cert", "/client-authentication/ee-certs"},
        {"ietf-tls-server", "client-auth-raw-public-key", "/client-authentication/raw-public-keys"},
        {"ietf-tls-server", "client-auth-tls12-psk", "/client-authentication/tls12-psks"},
        {"ietf-tls-server", "client-auth-tls13-epsk", "/client-authentication/tls13-epsks"},
        {"ietf-tls-common", "hello-params", "/hello-params"},
        {"ietf-tls-server", "tls-server-keepalives", "/keepalives"},
    };
    for (const auto &[module_name, feature_name, path] : UNSUPPORTED)
    {
        auto module = sess.getContext().getModuleImplemented(module_name);
        if (module && module->featureEnabled(feature_name))
        {
            reject_tls_settings(data, tls + path);
        }
    }
}

TlsMaterial load_tls_material(sysrepo::Session &sess)
{
    // one sysrepo request gives a consistent view across modules and prevents
    // concurrent commits from mixing references with another configuration's
    // keys, certificates or trust anchors
    auto snapshot = sess.getData("/ietf-keystore:keystore | /ietf-truststore:truststore | "
                                 "/sysrepo-gnxi-server:server/tls");
    if (!snapshot.has_value())
    {
        throw std::runtime_error("TLS configuration not found");
    }
    const auto &data = snapshot.value();

    TlsMaterial tls;
    check_unsupported_tls_config(sess, data);

    // the referenced key/certificate/bag names fluctuate, read them first
    std::string key_name = config_value(data,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/asymmetric-key",
        "TLS server identity asymmetric-key reference");
    std::string cert_name = config_value(data,
        "/sysrepo-gnxi-server:server/tls/server-identity/certificate/"
        "central-keystore-reference/certificate",
        "TLS server identity certificate reference");
    std::string bag_name = config_value(data,
        "/sysrepo-gnxi-server:server/tls/client-authentication/ca-certs/"
        "central-truststore-reference",
        "TLS client authentication CA-certs truststore reference");

    // private key of the server identity (only an X.509 key wrapped in PEM markers)
    // CMS is not supported
    std::string key_path = config_named_path(
        data, "/ietf-keystore:keystore/asymmetric-keys/asymmetric-key", key_name, "asymmetric-key");
    tls.private_key_pem =
        pem_wrap_private_key(config_value(data, key_path + "/cleartext-private-key",
                                 "cleartext-private-key of asymmetric-key '" + key_name + "'"),
            config_value(data, key_path + "/private-key-format",
                "private-key-format of asymmetric-key '" + key_name + "'"));

    // certificate of the server identity (only an X.509 key wrapped in PEM markers)
    // CMS is not supported
    std::string certificate_path = config_named_path(data, key_path + "/certificates/certificate",
        cert_name, "certificate '" + cert_name + "' of asymmetric-key '" + key_name + "'");
    tls.certificate_pem = pem_wrap_certificate(config_value(data, certificate_path + "/cert-data",
        "certificate '" + cert_name + "' of asymmetric-key '" + key_name + "'"));

    // CA certificates for client verification (only an X.509 certificate wrapped in PEM markers)
    // CMS is not supported
    std::string bag_path =
        config_named_path(data, "/ietf-truststore:truststore/certificate-bags/certificate-bag",
            bag_name, "certificate-bag");
    for (auto node : config_values(data, bag_path + "/certificate/cert-data"))
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
