/**
 * @file credentials.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief TLS server credentials implementation
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

#include "credentials.h"

#include <condition_variable>
#include <functional>
#include <queue>
#include <stdexcept>
#include <sysrepo-cpp/Enum.hpp>
#include <tuple>
#include <utility>
#include <vector>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/string_ref.h>
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Changes.hpp>

#include "utils/log.h"

/**
 * @brief grpc::AuthContext forwarding to the C API.
 *
 * grpc's own implementation is internal, the C API is used instead.
 * Property iteration is not implemented, no code path of this project
 * iterates the auth context of a metadata processor call.
 */
class CAuthContext final : public grpc::AuthContext
{
  public:
    explicit CAuthContext(grpc_auth_context *ctx) : ctx_(ctx) {}

    bool IsPeerAuthenticated() const override
    {
        return grpc_auth_context_peer_is_authenticated(ctx_) != 0;
    }

    std::vector<grpc::string_ref> GetPeerIdentity() const override
    {
        std::vector<grpc::string_ref> identity;
        auto it = grpc_auth_context_peer_identity(ctx_);
        const grpc_auth_property *property;
        while ((property = grpc_auth_property_iterator_next(&it)) != nullptr)
        {
            identity.emplace_back(property->value, property->value_length);
        }
        return identity;
    }

    std::string GetPeerIdentityPropertyName() const override
    {
        auto name = grpc_auth_context_peer_identity_property_name(ctx_);
        return name == nullptr ? "" : name;
    }

    std::vector<grpc::string_ref> FindPropertyValues(const std::string &name) const override
    {
        std::vector<grpc::string_ref> values;
        auto it = grpc_auth_context_find_properties_by_name(ctx_, name.c_str());
        const grpc_auth_property *property;
        while ((property = grpc_auth_property_iterator_next(&it)) != nullptr)
        {
            values.emplace_back(property->value, property->value_length);
        }
        return values;
    }

    grpc::AuthPropertyIterator begin() const override { return CAuthPropertyIterator(); }

    grpc::AuthPropertyIterator end() const override { return CAuthPropertyIterator(); }

    void AddProperty(const std::string &key, const grpc::string_ref &value) override
    {
        grpc_auth_context_add_property(ctx_, key.c_str(), value.data(), value.size());
    }

    bool SetPeerIdentityPropertyName(const std::string &name) override
    {
        return grpc_auth_context_set_peer_identity_property_name(ctx_, name.c_str()) != 0;
    }

  private:
    /**
     * @brief grpc::AuthPropertyIterator constructible without the internal
     * grpc friendship, yielding no properties.
     */
    class CAuthPropertyIterator final : public grpc::AuthPropertyIterator
    {
      public:
        CAuthPropertyIterator() : grpc::AuthPropertyIterator() {}
    };

    grpc_auth_context *ctx_;
};

/**
 * @brief Dynamically growing executor for blocking authentication processors.
 *
 * Mirrors the scheduling behavior of gRPC's default authentication processor
 * thread pool, one initial worker is created and another one is added whenever
 * work arrives while every existing worker is busy. Destruction drains queued
 * work and joins all workers.
 */
class ProcessorExecutor final
{
  public:
    /**
     * @brief Start the initial worker.
     */
    ProcessorExecutor() { threads_.emplace_back(&ProcessorExecutor::run_, this); }

    /**
     * @brief Drain queued work and stop all workers.
     */
    ~ProcessorExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto &thread : threads_)
        {
            thread.join();
        }
    }

    /**
     * @brief Queue a task, adding a worker when all existing workers are busy.
     *
     * @param[in] task Task to execute.
     */
    void add(std::function<void()> task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_)
        {
            throw std::runtime_error("authentication processor executor is shutting down");
        }
        if (waiting_ == 0)
        {
            threads_.emplace_back(&ProcessorExecutor::run_, this);
        }
        tasks_.push(std::move(task));
        cv_.notify_one();
    }

  private:
    /**
     * @brief Execute tasks until shutdown and the queue are both finished.
     */
    void run_()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                waiting_++;
                cv_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });
                waiting_--;
                if (tasks_.empty())
                {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    size_t waiting_ = 0;
    bool shutdown_ = false;
};

/**
 * @brief State of the C authentication metadata processor shim.
 */
struct ProcessorState
{
    /**
     * @brief Create the processor state and, for a blocking processor, its executor.
     *
     * @param[in] processor C++ authentication metadata processor.
     */
    explicit ProcessorState(const std::shared_ptr<grpc::AuthMetadataProcessor> &processor)
        : processor(processor)
    {
        if (this->processor && this->processor->IsBlocking())
        {
            executor = std::make_unique<ProcessorExecutor>();
        }
    }

    std::shared_ptr<grpc::AuthMetadataProcessor> processor;

    // destroyed before the processor, draining any tasks that still use it
    std::unique_ptr<ProcessorExecutor> executor;
};

/**
 * @brief Convert an output metadata entry for the done callback.
 *
 * The strings live in the metadata maps of the caller, which outlive the
 * done callback, so the slices just reference them.
 *
 * @param[in] key Metadata key.
 * @param[in] value Metadata value.
 * @return grpc_metadata entry.
 */
static grpc_metadata to_c_metadata(const std::string &key, const std::string &value)
{
    grpc_metadata entry{};
    entry.key = grpc_slice_from_static_buffer(key.data(), key.size());
    entry.value = grpc_slice_from_static_buffer(value.data(), value.size());
    return entry;
}

/**
 * @brief Complete authentication processing with an internal error.
 *
 * @param[in] cb Done callback.
 * @param[in] user_data User data of the done callback.
 */
static void processor_fail(grpc_process_auth_metadata_done_cb cb, void *user_data) noexcept
{
    cb(user_data, nullptr, 0, nullptr, 0, GRPC_STATUS_INTERNAL, nullptr);
}

/**
 * @brief Invoke the C++ processor and pass its output to the done callback.
 *
 * The C core retains the context and input metadata until the done callback
 * returns, including when this function runs on the executor. The callback
 * consumes the output arrays before returning.
 *
 * @param[in] processor C++ authentication metadata processor.
 * @param[in] context Auth context to store the username in.
 * @param[in] md Call credentials metadata.
 * @param[in] num_md Number of the metadata entries.
 * @param[in] cb Done callback.
 * @param[in] user_data User data of the done callback.
 */
static void processor_invoke(const std::shared_ptr<grpc::AuthMetadataProcessor> &processor,
    grpc_auth_context *context, const grpc_metadata *md, size_t num_md,
    grpc_process_auth_metadata_done_cb cb, void *user_data) noexcept
{
    try
    {
        grpc::AuthMetadataProcessor::InputMetadata metadata;
        for (size_t i = 0; i < num_md; i++)
        {
            metadata.emplace(
                grpc::string_ref(reinterpret_cast<const char *>(GRPC_SLICE_START_PTR(md[i].key)),
                    GRPC_SLICE_LENGTH(md[i].key)),
                grpc::string_ref(reinterpret_cast<const char *>(GRPC_SLICE_START_PTR(md[i].value)),
                    GRPC_SLICE_LENGTH(md[i].value)));
        }

        CAuthContext auth_context(context);
        grpc::AuthMetadataProcessor::OutputMetadata consumed_metadata, response_metadata;
        auto status =
            processor->Process(metadata, &auth_context, &consumed_metadata, &response_metadata);

        std::vector<grpc_metadata> consumed_md;
        consumed_md.reserve(consumed_metadata.size());
        for (const auto &[key, value] : consumed_metadata)
        {
            consumed_md.push_back(to_c_metadata(key, value));
        }
        std::vector<grpc_metadata> response_md;
        response_md.reserve(response_metadata.size());
        for (const auto &[key, value] : response_metadata)
        {
            response_md.push_back(to_c_metadata(key, value));
        }

        cb(user_data, consumed_md.data(), consumed_md.size(), response_md.data(),
            response_md.size(), static_cast<grpc_status_code>(status.error_code()),
            status.error_message().c_str());
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR("Authentication metadata processing failed: ", exc.what());
        processor_fail(cb, user_data);
    }
    catch (...)
    {
        SLOG_ERROR("Authentication metadata processing failed with an unknown exception");
        processor_fail(cb, user_data);
    }
}

/**
 * @brief C entry point of the authentication metadata processor.
 *
 * Like gRPC's internal wrapper, blocking processors are dispatched to an
 * executor while non-blocking processors are invoked directly.
 *
 * @param[in] state Processor state.
 * @param[in] context Auth context to store the username in.
 * @param[in] md Call credentials metadata.
 * @param[in] num_md Number of the metadata entries.
 * @param[in] cb Done callback.
 * @param[in] user_data User data of the done callback.
 */
static void processor_process(void *state, grpc_auth_context *context, const grpc_metadata *md,
    size_t num_md, grpc_process_auth_metadata_done_cb cb, void *user_data)
{
    auto *processor_state = static_cast<ProcessorState *>(state);
    auto &processor = processor_state->processor;
    if (!processor)
    {
        cb(user_data, nullptr, 0, nullptr, 0, GRPC_STATUS_OK, nullptr);
        return;
    }

    try
    {
        if (processor->IsBlocking())
        {
            processor_state->executor->add([processor, context, md, num_md, cb, user_data]
                { processor_invoke(processor, context, md, num_md, cb, user_data); });
        }
        else
        {
            processor_invoke(processor, context, md, num_md, cb, user_data);
        }
    }
    catch (const std::exception &exc)
    {
        SLOG_ERROR("Failed to schedule authentication metadata processing: ", exc.what());
        processor_fail(cb, user_data);
    }
    catch (...)
    {
        SLOG_ERROR(
            "Failed to schedule authentication metadata processing with an unknown exception");
        processor_fail(cb, user_data);
    }
}

/**
 * @brief Destroy the shim state together with the C++ processor.
 *
 * @param[in] state Processor state.
 */
static void processor_destroy(void *state)
{
    delete static_cast<ProcessorState *>(state);
}

/**
 * @brief grpc::ServerCredentials wrapping the C fetcher credentials.
 *
 * Newer gRPC (1.63 and up) takes the ownership of the C credentials in a
 * protected constructor, older gRPC provides no way to hand them to the
 * base class, so the private virtual AddPortToServer, called by the
 * server builder when the server is built, passes them to
 * grpc_server_add_http2_port directly and they are released in the
 * destructor.
 *
 * The authentication metadata processor is always installed through the C
 * API, the base class implementation of newer gRPC aborts, gRPC
 * implements it only in its internal SecureServerCredentials.
 */
class FetcherCredentials final : public grpc::ServerCredentials
{
  public:
    explicit FetcherCredentials(grpc_server_credentials *creds)
#ifdef GNXI_GRPC_SERVER_CREDENTIALS_CTOR
        : grpc::ServerCredentials(creds), c_creds_(creds)
#else
        : c_creds_(creds)
#endif
    {
    }

    // newer gRPC releases the credentials in its base class destructor
    ~FetcherCredentials() override
    {
#ifndef GNXI_GRPC_SERVER_CREDENTIALS_CTOR
        grpc_server_credentials_release(c_creds_);
#endif
    }

    /**
     * @brief Install the C++ processor through the C API shim.
     *
     * @param[in] processor C++ authentication metadata processor.
     */
    void SetAuthMetadataProcessor(
        const std::shared_ptr<grpc::AuthMetadataProcessor> &processor) override
    {
        grpc_auth_metadata_processor c_processor;
        c_processor.state = new ProcessorState{processor};
        c_processor.process = &processor_process;
        c_processor.destroy = &processor_destroy;
        grpc_server_credentials_set_auth_metadata_processor(c_creds_, c_processor);
    }

  private:
    int AddPortToServer(const std::string &addr, grpc_server *server) override
    {
        return grpc_server_add_http2_port(server, addr.c_str(), c_creds_);
    }

    grpc_server_credentials *c_creds_;
};

/**
 * @brief Dummy service of the trial server, gRPC cannot build a server
 * without any service registered.
 */
class TrialService final : public gnmi::gNMI::Service
{
  public:
    grpc::Status Capabilities(
        grpc::ServerContext *, const gnmi::CapabilityRequest *, gnmi::CapabilityResponse *) override
    {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "TLS candidate trial server");
    }
};

/**
 * @brief Check that gRPC itself would serve the candidate TLS material.
 *
 * No material is applied here, a trial credentials object and a trial
 * server on an ephemeral port are built instead, the certificate
 * configuration and then the server build go through the same gRPC
 * factory creation as a real connection setup would, so unusable material
 * fails here and can be rejected in the change callback.
 *
 * @param[in] material Candidate TLS material.
 * @return Error description, empty when the material can be served.
 */
static std::optional<std::string> trial_validate(const TlsMaterial &material)
{
    grpc_ssl_pem_key_cert_pair key_cert_pair = {
        material.private_key_pem.c_str(), material.certificate_pem.c_str()};
    std::unique_ptr<grpc_ssl_server_certificate_config,
        void (*)(grpc_ssl_server_certificate_config *)>
        config(grpc_ssl_server_certificate_config_create(
                   material.ca_certificate_pem.c_str(), &key_cert_pair, 1),
            grpc_ssl_server_certificate_config_destroy);
    if (config == nullptr)
    {
        return "gRPC cannot parse new TLS certificate configuration";
    }

    auto *options = grpc_ssl_server_credentials_create_options_using_config(
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY, config.get());
    // the options object takes over the certificate configuration
    std::ignore = config.release();
    std::unique_ptr<grpc_server_credentials, void (*)(grpc_server_credentials *)> creds(
        grpc_ssl_server_credentials_create_with_options(options), grpc_server_credentials_release);
    if (creds == nullptr)
    {
        return "gRPC cannot create new server credentials";
    }

    auto wrapper = std::make_shared<FetcherCredentials>(creds.get());
    // the trial credentials wrapper takes over the ownership
    std::ignore = creds.release();

    TrialService trial_service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", wrapper);
    builder.RegisterService(&trial_service);
    auto trial_server = builder.BuildAndStart();
    if (trial_server == nullptr)
    {
        return "gRPC cannot create a server with the new TLS material";
    }
    return std::nullopt;
}

ReloadableCredentials::ReloadableCredentials(sysrepo::Connection conn) : conn_(std::move(conn))
{
    subscription_.emplace(conn_.sessionStart(sysrepo::Datastore::Running));
    auto on_change = [this](sysrepo::Session session, uint32_t sub_id, std::string_view module_name,
                         std::optional<std::string_view> xpath, sysrepo::Event event,
                         uint32_t request_id)
    {
        (void)sub_id;
        (void)module_name;
        (void)xpath;
        return on_material_change(session, event, request_id);
    };
    subscription_->data_change_subscribe(
        on_change, "/ietf-keystore:keystore", 0, sysrepo::SubscribeOptions::ChangesForAllModules);
    subscription_->data_change_subscribe(on_change, "/ietf-truststore:truststore", 0,
        sysrepo::SubscribeOptions::ChangesForAllModules);
    subscription_->data_change_subscribe(on_change, "/sysrepo-gnxi-server:server/tls", 0,
        sysrepo::SubscribeOptions::ChangesForAllModules);

    // subscribe before the initial load so no change can be missed, keep the
    // lock until the assignment so a newer callback result cannot be overwritten
    {
        auto sess = conn_.sessionStart(sysrepo::Datastore::Running);
        std::lock_guard<std::mutex> lock(mutex_);
        material_ = load_tls_material(sess);
    }

    auto *options = grpc_ssl_server_credentials_create_options_using_config_fetcher(
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY, &fetch_config, this);
    std::unique_ptr<grpc_server_credentials, void (*)(grpc_server_credentials *)> creds(
        grpc_ssl_server_credentials_create_with_options(options), grpc_server_credentials_release);
    if (creds == nullptr)
    {
        throw std::runtime_error("creation of the reloadable server credentials failed");
    }
    credentials_ = std::make_shared<FetcherCredentials>(creds.get());
    // the C credentials are now owned by FetcherCredentials
    std::ignore = creds.release();
}

ReloadableCredentials::~ReloadableCredentials()
{
    // unsubscribe before the members the callbacks use are destroyed
    subscription_.reset();
}

std::shared_ptr<grpc::ServerCredentials> ReloadableCredentials::credentials() const
{
    return credentials_;
}

grpc_ssl_certificate_config_reload_status ReloadableCredentials::fetch_config(
    void *user_data, grpc_ssl_server_certificate_config **config)
{
    auto *self = static_cast<ReloadableCredentials *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);

    // always build a fresh config, gRPC compares it with the one in use
    // itself and skips the reload when nothing changed
    grpc_ssl_pem_key_cert_pair key_cert_pair = {
        self->material_.private_key_pem.c_str(), self->material_.certificate_pem.c_str()};
    *config = grpc_ssl_server_certificate_config_create(
        self->material_.ca_certificate_pem.c_str(), &key_cert_pair, 1);
    if (*config == nullptr)
    {
        SLOG_ERROR("Failed to build the SSL server certificate configuration, "
                   "the previously loaded TLS material is kept");
        return GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_FAIL;
    }
    return GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_NEW;
}

sysrepo::ErrorCode ReloadableCredentials::on_material_change(
    sysrepo::Session session, sysrepo::Event event, uint32_t request_id)
{
    switch (event)
    {
    case sysrepo::Event::Change:
        if (validated_operation_ == request_id)
        {
            // another module of this same operation was already validated
            return sysrepo::ErrorCode::Ok;
        }

        // the callback session exposes the candidate configuration directly
        try
        {
            auto candidate = load_tls_material(session);
            if (auto error = trial_validate(candidate))
            {
                // a change that cannot be served must not be committed
                session.setErrorMessage(*error);
                return sysrepo::ErrorCode::CallbackFailed;
            }
            validated_operation_ = request_id;
        }
        catch (const std::exception &exc)
        {
            session.setErrorMessage(exc.what());
            return sysrepo::ErrorCode::CallbackFailed;
        }
        return sysrepo::ErrorCode::Ok;
    case sysrepo::Event::Done:
        if (applied_operation_ == request_id)
        {
            // another module of this same operation was already applied
            return sysrepo::ErrorCode::Ok;
        }

        try
        {
            auto material = load_tls_material(session);
            std::lock_guard<std::mutex> lock(mutex_);
            if (material != material_)
            {
                material_ = std::move(material);
                SLOG_INFO("TLS server material updated, new connections will use it");
            }
            applied_operation_ = request_id;
        }
        catch (const std::exception &exc)
        {
            SLOG_ERROR("Failed to apply the new TLS material, the server is out of sync "
                       "with the datastore: ",
                exc.what());
        }
        return sysrepo::ErrorCode::Ok;
    case sysrepo::Event::Abort:
        // the material is only loaded into the cache after the done event, so
        // there is nothing to revert here
        return sysrepo::ErrorCode::Ok;
    default:
        return sysrepo::ErrorCode::Ok;
    }
}
