/**
 * @file tls.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief TLS material loading header
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

#include <string>

#include <sysrepo-cpp/Session.hpp>

/**
 * @brief Server TLS material (the base64 bodies stored in sysrepo wrapped
 * in PEM markers for gRPC).
 */
struct TlsMaterial
{
    std::string private_key_pem;
    std::string certificate_pem;
    std::string ca_certificate_pem;

    bool operator==(const TlsMaterial &) const = default;
};

/**
 * @brief Load the TLS server material from sysrepo. Throw on error.
 *
 * Reads the server identity (private key and certificate chain from
 * ietf-keystore) and the client verification CA bundle (ietf-truststore)
 * referenced by the server configuration. The stored base64 bodies are
 * wrapped into PEM for gRPC.
 *
 * @param[in] sess Sysrepo session.
 * @return TLS material wrapped into PEM.
 */
TlsMaterial load_tls_material(sysrepo::Session &sess);
