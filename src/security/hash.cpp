/**
 * @file hash.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Hash handling implementation
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
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "hash.h"

constexpr size_t SALT_LENGTH = 16;

/**
 * @brief Encode a part of the password in the hex format in order to store it.
 *
 * @param[in] data Binary data to encode.
 * @param[in] len Length of the @p data data.
 * @return Binary data encoded in the hex format.
 */
std::string hex_encode(const unsigned char *data, size_t len)
{
    // OpenSSL 3.0+ API writes null terminated uppercase hex
    // reserve an extra byte for the null and strip it afterwards
    std::string out(len * 2 + 1, '\0');
    size_t out_len = 0;
    if (OPENSSL_buf2hexstr_ex(out.data(), out.size(), &out_len, data, len, '\0') != 1)
    {
        throw std::runtime_error("OPENSSL_buf2hexstr_ex failed");
    }
    out.resize(out_len - 1);
    return out;
}

/**
 * @brief Decode a part of the password from the hex format in order to compare it.
 *
 * @param[in] hex Stored hex to decode.
 * @param[in] out Decoded binary data on success.
 * @return True if the data were successfully decoded, false otherwise.
 */
bool hex_decode(const std::string &hex, std::vector<unsigned char> &out)
{
    // check in case of db corruption
    if (hex.empty() || hex.size() % 2 != 0)
    {
        return false;
    }
    out.assign(hex.size() / 2, 0);
    size_t out_len = 0;
    // fails on odd-length leftovers and non-hex characters
    if (OPENSSL_hexstr2buf_ex(out.data(), out.size(), &out_len, hex.c_str(), '\0') != 1)
    {
        return false;
    }
    out.resize(out_len);
    return true;
}

/**
 * @brief Get a pointer to the structure representing the @p algo algorithm.
 *
 * @param[in] algo Algorithm name.
 * @return Algorithm (for the password hash).
 */
const EVP_MD *get_algo(const std::string &algo)
{
    // convenience alias
    if (algo == "sha2")
    {
        return EVP_sha256();
    }
    return EVP_get_digestbyname(algo.c_str());
}

/**
 * @brief Compute the password hash.
 *
 * @param[in] md Algorithm to use.
 * @param[in] salt Random salt to add.
 * @param[in] password Password to hash.
 * @return Password hash.
 */
std::vector<unsigned char> compute_hash(const EVP_MD *md, const std::vector<unsigned char> &salt,
                                        const std::string &password)
{
    std::vector<unsigned char> out(EVP_MAX_MD_SIZE);
    std::vector<unsigned char> buf;
    buf.reserve(salt.size() + password.size());
    buf.insert(buf.end(), salt.begin(), salt.end());
    buf.insert(buf.end(), password.begin(), password.end());
    unsigned int out_len = 0;
    if (EVP_Digest(buf.data(), buf.size(), out.data(), &out_len, md, nullptr) != 1)
    {
        throw std::runtime_error("EVP_Digest failed");
    }
    out.resize(out_len);
    return out;
}

/**
 * @brief Compare hashes.
 *
 * @param[in] a First hash.
 * @param[in] b Second hash.
 * @return True if the hashes are equal, false otherwise.
 */
bool compare_hashes(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

/**
 * @brief Compare plaintext passwords.
 *
 * @param[in] a First password.
 * @param[in] b Second password.
 * @return True if the passwords are equal, false otherwise.
 */
bool compare_plaintext(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string make_hash(const std::string &password, const std::string &algo)
{
    // if no algorithm was specified store as plaintext
    if (algo.empty())
    {
        return "plaintext$" + password;
    }

    const EVP_MD *md = get_algo(algo);
    if (!md)
    {
        throw std::runtime_error("Unknown hash algorithm: " + algo);
    }

    std::vector<unsigned char> salt(SALT_LENGTH);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
    {
        throw std::runtime_error("RAND_bytes failed");
    }

    auto digest = compute_hash(md, salt, password);

    return algo + "$" + hex_encode(salt.data(), salt.size()) + "$" +
           hex_encode(digest.data(), digest.size());
}

bool check_hash(const std::string &password, const std::string &stored)
{
    // <algorithm>$...
    auto p1 = stored.find('$');
    if (p1 == std::string::npos || p1 == 0)
    {
        return false;
    }
    std::string algo = stored.substr(0, p1);
    if (algo == "plaintext")
    {
        // substr(p1+1) cannot throw out of range (pos == size() is allowed and yields empty string)
        return compare_plaintext(password, stored.substr(p1 + 1));
    }

    // ...<salt>$...
    auto p2 = stored.find('$', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1)
    {
        return false;
    }
    std::string salt_hex = stored.substr(p1 + 1, p2 - p1 - 1);

    // ...<digest>
    std::string digest_hex = stored.substr(p2 + 1);
    if (digest_hex.empty())
    {
        return false;
    }

    const EVP_MD *md = get_algo(algo);
    if (!md)
    {
        // unknown algorithm
        return false;
    }

    // decode stored salt and digest (password hash)
    std::vector<unsigned char> salt, expected;
    if (!hex_decode(salt_hex, salt) || !hex_decode(digest_hex, expected))
    {
        return false;
    }

    // compute hash with the retrieved salt and compare
    std::vector<unsigned char> actual;
    try
    {
        actual = compute_hash(md, salt, password);
    }
    catch (const std::exception &)
    {
        return false;
    }

    return compare_hashes(actual, expected);
}
