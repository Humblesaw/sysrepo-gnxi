/**
 * @file crypto.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief PEM marker string utilities header
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

/**
 * @brief Wrap a base64(DER) X.509 certificate body in PEM markers.
 *
 * @param[in] base64 Base64 certificate body (no markers, no newlines).
 * @return PEM-encoded certificate.
 */
std::string pem_wrap_certificate(const std::string &base64);

/**
 * @brief Wrap a base64(DER) private key body in PEM markers. Throws on error.
 *
 * @param[in] base64 Base64 private key body (no markers, no newlines).
 * @param[in] format_identity private-key-format identity value.
 * @return PEM-encoded private key.
 */
std::string pem_wrap_private_key(const std::string &base64, const std::string &format_identity);
