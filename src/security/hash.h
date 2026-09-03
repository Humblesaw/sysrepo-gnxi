/**
 * @file hash.h
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Hash handling header
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
 * @brief Hash password with a random salt using the crypt(3) algorithms
 * modeled by the iana-crypt-hash YANG module: MD5-crypt ($1$),
 * SHA-256-crypt ($5$) or SHA-512-crypt ($6$), computed by libcrypt.
 * Throws error on unknown algorithm or hashing failure.
 *
 * @param[in] password Password to be hashed.
 * @param[in] algo Hashing algorithm to use: "md5", "sha256" or "sha512"
 *                 (used when @p algo is empty).
 * @return Password hash in the crypt-hash format, e.g. "$6$<salt>$<hash>".
 */
std::string make_hash(const std::string &password, const std::string &algo);

/**
 * @brief Verify a password against a stored crypt-hash value. The '$0$'
 * cleartext form is an unsupported crypt(3) setting - it never
 * authenticates (the schema rejects storing it as well).
 *
 * @param[in] password Password to be checked.
 * @param[in] stored Stored hash to compare against.
 * @return True if password produced the same hash, false otherwise
 *         (including for malformed stored values).
 */
bool check_hash(const std::string &password, const std::string &stored);
