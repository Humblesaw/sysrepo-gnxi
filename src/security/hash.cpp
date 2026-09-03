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

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include <crypt.h>

#include <openssl/crypto.h>

#include "hash.h"

namespace
{

/**
 * @brief Compare two hashes.
 *
 * @param[in] a First string.
 * @param[in] b Second string.
 * @return True if the hashes obtained from the strings are equal, false otherwise.
 */
bool secure_equals(std::string_view a, std::string_view b)
{
    // compare only the hash parts, the recomputed setting may be
    // normalized by libcrypt and differ from the stored value (this
    // also rejects values not in the modular crypt format)
    auto last_dollar_a = a.rfind('$');
    auto last_dollar_b = b.rfind('$');
    if (last_dollar_a == std::string_view::npos || last_dollar_b == std::string_view::npos)
    {
        return false;
    }
    auto only_hash_a = a.substr(last_dollar_a + 1);
    auto only_hash_b = b.substr(last_dollar_b + 1);
    if (only_hash_a.size() != only_hash_b.size())
    {
        return false;
    }
    return CRYPTO_memcmp(only_hash_a.data(), only_hash_b.data(), only_hash_a.size()) == 0;
}

/**
 * @brief RAII wrapper for struct crypt_data. The structure is
 * heap-allocated as it is over 32 KiB large, and it is zeroed out before
 * the memory is released since it holds password-derived data.
 */
class CryptData
{
  public:
    CryptData()
        : m_data(static_cast<struct crypt_data *>(std::calloc(1, sizeof(struct crypt_data))))
    {
        if (!m_data)
        {
            throw std::runtime_error("calloc failed");
        }
    }

    ~CryptData()
    {
        OPENSSL_cleanse(m_data, sizeof(struct crypt_data));
        std::free(m_data);
    }

    CryptData(const CryptData &) = delete;
    CryptData &operator=(const CryptData &) = delete;

    struct crypt_data *get() { return m_data; }

  private:
    struct crypt_data *m_data;
};

/**
 * @brief Hash @p password for the crypt(3) @p setting.
 *
 * @param[in] password Password to hash.
 * @param[in] setting crypt(3) setting string ("$<id>$[rounds=<N>$]<salt>...").
 * @return Full crypt-hash string, e.g. "$6$<salt>$<hash>".
 * @throws std::runtime_error if the hashing fails (unsupported setting).
 */
std::string crypt_hash(const std::string &password, const std::string &setting)
{
    CryptData data;
    char *hash = crypt_r(password.c_str(), setting.c_str(), data.get());
    if (!hash)
    {
        throw std::runtime_error("crypt_r failed");
    }
    return std::string(hash);
}

} // namespace

std::string make_hash(const std::string &password, const std::string &algo)
{
    const char *prefix;

    if (algo.empty() || algo == "sha512")
    {
        prefix = "$6$";
    }
    else if (algo == "sha256")
    {
        prefix = "$5$";
    }
    else if (algo == "md5")
    {
        prefix = "$1$";
    }
    else
    {
        throw std::runtime_error("Unknown hash algorithm: " + algo);
    }

    // generate a setting with a random salt (entropy is taken from the
    // operating system) and the default number of rounds
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn(prefix, 0, nullptr, 0, setting, sizeof(setting)))
    {
        throw std::runtime_error("crypt_gensalt_rn failed");
    }

    return crypt_hash(password, setting);
}

bool check_hash(const std::string &password, const std::string &stored)
{
    try
    {
        auto computed = crypt_hash(password, stored);
        auto matches = secure_equals(computed, stored);
        OPENSSL_cleanse(computed.data(), computed.size());
        return matches;
    }
    catch (const std::exception &)
    {
        // hashing failure or malformed stored value
        return false;
    }
}
