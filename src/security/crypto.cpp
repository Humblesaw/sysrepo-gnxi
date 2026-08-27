/**
 * @file crypto.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief PEM marker string utilities implementation
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

#include <stdexcept>

#include "crypto.h"

std::string pem_wrap_certificate(const std::string &base64)
{
    return "-----BEGIN CERTIFICATE-----\n" + base64 + "\n-----END CERTIFICATE-----";
}

std::string pem_wrap_private_key(const std::string &base64, const std::string &format_identity)
{
    std::string label;
    if (format_identity == "ietf-crypto-types:rsa-private-key-format")
    {
        label = "RSA ";
    }
    else if (format_identity == "ietf-crypto-types:ec-private-key-format")
    {
        label = "EC ";
    }
    else if (format_identity == "ietf-crypto-types:one-asymmetric-key-format")
    {
        label = "";
    }
    else
    {
        throw std::runtime_error("unsupported private-key-format identity: " + format_identity);
    }

    return "-----BEGIN " + label + "PRIVATE KEY-----\n" + base64 + "\n-----END " + label +
           "PRIVATE KEY-----";
}
