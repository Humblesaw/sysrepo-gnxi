/*
 * Copyright 2020 Yohan Pipereau
 * Copyright 2025 Graphiant Inc.
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

#include <fstream>

#include "utils/log.h"

#include "authentication.h"

/* GetFileContent - Get an entier File content
 * @param Path to the file
 * @return String containing the entire File.
 */
static std::string GetFileContent(std::string path)
{
    std::ifstream ifs(path);
    if (!ifs)
    {
        SLOG_FATAL("File ", path, " not found");
        exit(1);
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

    ifs.close();

    return content;
}

/* SslCredentialsHelper -
 * @param ppath Private Key path
 * @param cpath certificates path
 * @param rpath root certificate path
 * @param client_cert boolean to activate/deactivate client certificate check
 * @return ServerCredentials for grpc service creation
 */
static std::shared_ptr<grpc::ServerCredentials>
SslCredentialsHelper(std::string ppath, std::string cpath, std::string rpath, bool client_cert)
{
    grpc::SslServerCredentialsOptions ssl_opts;

    if (client_cert)
    {
        ssl_opts.client_certificate_request =
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    }
    else
    {
        ssl_opts.client_certificate_request = GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    }

    grpc::SslServerCredentialsOptions::PemKeyCertPair pkcp = {GetFileContent(ppath),
                                                              GetFileContent(cpath)};

    ssl_opts.pem_key_cert_pairs.push_back(pkcp);

    // Require client root certificates to avoid grpc bug in versions < 1.18.0
    // (https://github.com/grpc/grpc/pull/17500)
    if (rpath.empty())
    {
        SLOG_FATAL("Client root certificates must be specified");
        exit(1);
    }

    ssl_opts.pem_root_certs = GetFileContent(rpath);

    return grpc::SslServerCredentials(ssl_opts);
}

/* Get Server Credentials according to Scurity Context policy */
std::shared_ptr<grpc::ServerCredentials> AuthBuilder::build()
{
    std::shared_ptr<grpc::ServerCredentials> cred;

    // MUTUAL_TLS
    if (!private_key_path.empty() && !cert_path.empty() && username.empty() && password.empty())
    {
        SLOG_INFO("Mutual TLS authentication");
        return SslCredentialsHelper(private_key_path, cert_path, root_cert_path, true);
    }

    // USERPASS_TLS
    if (!private_key_path.empty() && !cert_path.empty() && !username.empty() && !password.empty())
    {
        SLOG_INFO("Username/Password over TLS authentication");
        cred = SslCredentialsHelper(private_key_path, cert_path, root_cert_path, false);
        cred->SetAuthMetadataProcessor(std::make_shared<UserPassAuthenticator>(username, password));
        return cred;
    }

    // INSECURE
    if (insecure)
    {
        SLOG_INFO("Insecure authentication");
        return grpc::InsecureServerCredentials();
    }

    /* impossible scenario */
    if (private_key_path.empty() && cert_path.empty() && !username.empty() && !password.empty())
        SLOG_FATAL("Impossible to use user/pass auth with insecure connection");

    SLOG_FATAL("Unsupported Authentication method");

    exit(1);
}

AuthBuilder &AuthBuilder::setKeyPath(std::string keyPath)
{
    private_key_path = keyPath;
    return *this;
}

AuthBuilder &AuthBuilder::setCertPath(std::string certPath)
{
    cert_path = certPath;
    return *this;
}

AuthBuilder &AuthBuilder::setRootCertPath(std::string rootpath)
{
    root_cert_path = rootpath;
    return *this;
}

AuthBuilder &AuthBuilder::setUsername(std::string user)
{
    username = user;
    return *this;
}

AuthBuilder &AuthBuilder::setPassword(std::string pass)
{
    password = pass;
    return *this;
}

AuthBuilder &AuthBuilder::setInsecure(bool mode)
{
    insecure = mode;
    return *this;
}

/* Implement a MetadataProcessor for username/password authentication */
grpc::Status UserPassAuthenticator::Process(const InputMetadata &auth_metadata,
                                            grpc::AuthContext *context,
                                            OutputMetadata *consumed_auth_metadata,
                                            OutputMetadata *response_metadata)
{
    (void)context;
    (void)response_metadata; // Unused

    /* Look for username/password fields in Metadata sent by client */
    auto user_kv = auth_metadata.find("username");
    if (user_kv == auth_metadata.end())
    {
        SLOG_ERROR("No username field");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No username field");
    }
    auto pass_kv = auth_metadata.find("password");
    if (pass_kv == auth_metadata.end())
    {
        SLOG_ERROR("No password field");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No password field");
    }

    /* test if username and password are good */
    if (password.compare(pass_kv->second.data()) != 0 ||
        username.compare(user_kv->second.data()) != 0)
    {
        SLOG_ERROR("Invalid username/password");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid username/password");
    }

    /* Remove username and password key-value from metadata */
    consumed_auth_metadata->insert(
        std::make_pair(std::string(user_kv->first.data(), user_kv->first.length()),
                       std::string(user_kv->second.data(), user_kv->second.length())));
    consumed_auth_metadata->insert(
        std::make_pair(std::string(pass_kv->first.data(), pass_kv->first.length()),
                       std::string(pass_kv->second.data(), pass_kv->second.length())));

    return grpc::Status::OK;
}
