/**
 * @file subscribe.cpp
 * @author Ondrej Kusnirik (kusnirik@cesnet.cz)
 * @brief Subscribe RPC implementation
 *
 * @copyright
 * Copyright 2020 Yohan Pipereau
 * Copyright 2025 Graphiant Inc.
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

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <libyang/set.h>
#include <libyang/tree_data.h>
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>
// #include <grpcpp/impl/codegen/core_codegen_interface.h>

#include "subscribe.h"
#include "utils/sysrepo.h"
#include <sysrepo-cpp/Changes.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <utils/log.h>
#include <utils/path.h>
#include <utils/utils.h>

namespace impl
{

namespace
{
// the lowest SAMPLE interval the server supports
constexpr auto MIN_SAMPLE_INTERVAL = std::chrono::milliseconds(200);
} // namespace

grpc::Status Subscribe::build_subs_update_(
    google::protobuf::RepeatedPtrField<gnmi::Update> *update_list, std::string fullpath,
    gnmi::Encoding encoding, std::string origin)
{
    gnmi::Update *update;

    SessionDsSwitcher ds_switch(sr_sess_, sysrepo::Datastore::Operational);

    try
    {
        // get multiple subtree for YANG lists or one for other YANG types
        std::optional<libyang::DataNode> sr_trees = sr_sess_.getData(fullpath.c_str());

        // the path not (yet) existing isn't an error, so just return an empty set
        if (!sr_trees.has_value())
        {
            return grpc::Status::OK;
        }

        for (libyang::DataNode n : sr_trees->findXPath(fullpath))
        {
            // root and wildcard paths may span modules of other origins
            if (!gnxi::data_node_in_origin(n, origin))
            {
                continue;
            }
            update = update_list->Add();
            gnxi::data_node_to_gnmi_path(
                n, gnxi::format_for_origin(origin), *update->mutable_path());
            update->mutable_path()->set_origin(origin);
            grpc::Status status = encodef_->encode(encoding, n, update->mutable_val());
            if (!status.ok())
            {
                update_list->Clear();
                return status;
            }
        }
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        update_list->Clear();
        SLOG_ERROR("Failed getting items from sysrepo: ", exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    catch (const std::runtime_error &exc)
    {
        SLOG_DEBUG("Path not found or not evaluable: \"", fullpath, "\": ", exc.what());
        update_list->Clear();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, exc.what());
    }

    return grpc::Status::OK;
}

grpc::Status Subscribe::build_subscribe_notification(
    gnmi::Notification *notification, const gnmi::SubscriptionList &request, bool *sample)
{
    google::protobuf::RepeatedPtrField<gnmi::Update> *update_list = notification->mutable_update();
    grpc::Status status;

    // check if only updates should be sent
    if (request.updates_only())
    {
        SLOG_WARN("Unsupported updates_only in SubscriptionList");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "updates-only not supported");
    }

    // get time since epoch in milliseconds
    notification->set_timestamp(get_time_nanosec());

    if (sample)
    {
        *sample = false;
    }

    // fill Update RepeatedPtrField in Notification message
    // Update field contains only data elements that have changed values
    for (int i = 0; i < request.subscription_size(); i++)
    {
        gnmi::Subscription sub = request.subscription(i);

        // on-change subscriptions get their initial data later, once the
        // change subscription is registered (TARGET_DEFINED is rejected)
        if (request.mode() == gnmi::SubscriptionList_Mode_STREAM &&
            sub.mode() == gnmi::SubscriptionMode::ON_CHANGE)
        {
            SLOG_DEBUG("On-change, getting initial data later: ", sub.path().ShortDebugString());
            continue;
        }
        if (sample)
        {
            *sample = true;
        }

        // fetch all found counters value for a requested path
        std::string origin;
        try
        {
            origin = gnxi::resolve_origin(request.prefix(), sub.path());
            auto fullpath =
                gnxi::gnmi_to_xpath(sr_sess_.getContext(), request.prefix(), sub.path());
            status = build_subs_update_(update_list, fullpath, request.encoding(), origin);
        }
        catch (const grpc::Status &path_status)
        {
            SLOG_WARN("Invalid subscription: ", path_status.error_message());
            return path_status;
        }
        if (!status.ok())
        {
            SLOG_WARN("Failed building update for ", sub.path().ShortDebugString());
            return status;
        }
    }

    // gNMI spec §2.2.2.1: the target set in the prefix of a request MUST be
    // reflected in the prefix of the corresponding response, and MUST NOT be
    // set when the request does not set it
    if (!request.prefix().target().empty())
    {
        notification->mutable_prefix()->set_target(request.prefix().target());
    }
    // the origin of the request prefix is echoed in the response prefix
    if (!request.prefix().origin().empty())
    {
        notification->mutable_prefix()->set_origin(request.prefix().origin());
    }

    notification->set_atomic(false);

    return grpc::Status::OK;
}

grpc::Status Subscribe::build_subscribe_notification_for_changes(gnmi::Notification *notification,
    const gnmi::SubscriptionList &request, std::string &xpath, std::string origin,
    sysrepo::Session session)
{
    auto update_list = notification->mutable_update();
    auto delete_list = notification->mutable_delete_();
    grpc::Status status;
    auto format = gnxi::format_for_origin(origin);

    // check if only updates should be sent
    if (request.updates_only())
    {
        SLOG_WARN("Unsupported updates_only in SubscriptionList");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "updates-only not supported");
    }

    // get time since epoch in milliseconds
    notification->set_timestamp(get_time_nanosec());

    // gNMI spec §2.2.2.1: the target set in the prefix of a request MUST be
    // reflected in the prefix of the corresponding response, and MUST NOT be
    // set when the request does not set it
    if (!request.prefix().target().empty())
    {
        notification->mutable_prefix()->set_target(request.prefix().target());
    }
    // the origin of the request prefix is echoed in the response prefix
    if (!request.prefix().origin().empty())
    {
        notification->mutable_prefix()->set_origin(origin);
    }

    // fill Update RepeatedPtrField in Notification message
    // Update field contains only data elements that have changed values

    // when the stream's session has a NACM user (secure mode), hide all
    // changes the recipient may not read, sysrepo's NACM covers data
    // reads/edits, RPCs and notifications, but NOT module-change
    // subscriptions, so the filtering must be applied explicitly per
    // recipient here
    const auto recipient_nacm_user = sr_nacm_get_user(sysrepo::getRawSession(sr_sess_));
    auto nacm_visible = [&](const libyang::DataNode &node)
    {
        if (!recipient_nacm_user)
        {
            return true;
        }

        // rebuild the changed node as a standalone tree with its ancestors
        // and let sysrepo filter it for the recipient
        const struct ly_ctx *raw_ctx = libyang::getRawNode(node)->schema->module->ctx;
        const std::string path = node.path();
        const char *value = node.isTerm() ? lyd_get_value(libyang::getRawNode(node)) : nullptr;

        struct lyd_node *filter_tree = nullptr, *created = nullptr;
        if (lyd_new_path2(nullptr, raw_ctx, path.c_str(), value, 0, 0, 0, &filter_tree, &created) !=
            LY_SUCCESS)
        {
            SLOG_WARN("NACM filter tree build failed for ", path, ", hiding the change");
            return false;
        }
        (void)created;

        bool visible =
            sr_nacm_filter_data_read(sysrepo::getRawSession(sr_sess_), &filter_tree) == SR_ERR_OK &&
            filter_tree != nullptr;
        if (visible)
        {
            // the node itself may have been filtered out while its ancestors remained
            struct ly_set *set = nullptr;
            visible = lyd_find_xpath2(filter_tree, path.c_str(), nullptr, &set) == LY_SUCCESS &&
                      set->count > 0;
            ly_set_free(set, nullptr);
        }
        lyd_free_all(filter_tree);
        return visible;
    };

    try
    {
        auto last_change = make_pair(std::string(""), sysrepo::ChangeOperation::Created);

        std::string changes_path(xpath);
        changes_path += "//.";
        auto iter = session.getChanges(changes_path.c_str());
        for (const auto &change : iter)
        {
            if (!last_change.first.empty() && last_change.second == change.operation &&
                // if we have the identifier of one leaf as a substring of another at the same
                // level, we can confuse between the two. for example searching for "ike-connection"
                // and finding "ike-connection-up". So search with "/" suffixed and prevent this
                // mix-up
                (change.node.path().rfind(last_change.first + "/", 0) == 0 ||
                    change.node.path() == last_change.first))
            {
                continue;
            }
            last_change = std::make_pair(change.node.path(), change.operation);

            // the subscription (module) may span data of other origins
            if (!gnxi::data_node_in_origin(change.node, origin))
            {
                continue;
            }

            // deliver changes only to recipients allowed to read them (NACM)
            if (!nacm_visible(change.node))
            {
                SLOG_DEBUG(
                    "Change to ", change.node.path(), " hidden from the stream recipient by NACM");
                continue;
            }

            auto val =
                change.node.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Siblings)
                    .value();
            SLOG_DEBUG("Subscribe notification, operation: ", change.operation,
                ", path: ", change.node.path(), ", value: ", val);

            // also done for updated nodes due to gNMI spec §3.5.2.3:
            // To replace the contents of an entire node within the tree, the target populates
            // the delete field with the path of the node being removed, along with the new
            // contents within the update field.
            if (change.operation != sysrepo::ChangeOperation::Created)
            {
                auto path_p = delete_list->Add();
                gnxi::data_node_to_gnmi_path(change.node, format, *path_p);
                path_p->set_origin(origin);
            }
            if (change.operation != sysrepo::ChangeOperation::Deleted)
            {
                auto update = update_list->Add();

                gnxi::data_node_to_gnmi_path(change.node, format, *update->mutable_path());
                update->mutable_path()->set_origin(origin);
                // remove all of the attributes from nodes which we don't need and may confuse
                // parsers of the JSON when using that encoding
                // auto opts = static_cast<uint32_t>(libyang::DuplicationOptions::NoMeta) |
                //             static_cast<uint32_t>(libyang::DuplicationOptions::Recursive);
                // auto node =
                // change.node.duplicate(static_cast<libyang::DuplicationOptions>(opts));
                status = encodef_->encode(request.encoding(), change.node, update->mutable_val());
                if (!status.ok())
                {
                    return status;
                }
            }
        }
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR("Failed processing module changes from sysrepo: ", exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    catch (const std::runtime_error &exc)
    {
        SLOG_WARN("Invalid subscription: ", exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }

    notification->set_atomic(false);

    return grpc::Status::OK;
}

void Subscribe::trigger_sample_update(grpc::ServerContext *context,
    std::shared_ptr<gnmi::Subscription> &sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    gnmi::Encoding encoding, const gnmi::Path &prefix)
{
    gnmi::SubscribeResponse response;
    gnmi::SubscriptionList update_list;

    // add the subscription entry to the subscription list
    update_list.add_subscription()->CopyFrom(*sub);
    // propagate the encoding of the original subscription list
    update_list.set_encoding(encoding);
    // propagate the prefix of the original subscription list - both the data
    // fetch and the emitted paths depend on it
    *update_list.mutable_prefix() = prefix;

    if (!context->IsCancelled())
    {
        grpc::Status status = build_subscribe_notification(response.mutable_update(), update_list);
        if (!status.ok())
        {
            // this is a hack to allow the Read in the parent thread to return,
            // but it avoids needing to move to an asynchronous model just to return this one error
            // grpc::g_core_codegen_interface->grpc_call_cancel_with_status(
            //   context->c_call(), static_cast<grpc_status_code>(status.error_code()),
            //   status.error_message().c_str(), nullptr);
            return;
        }
        write(stream, response);
        response.Clear();
    }
}

/**
 * @brief A timed sample-subscription event: expiry time and the coroutine
 * handle to resume.
 *
 */
struct SampleSubscriptionEvent
{
    std::chrono::steady_clock::time_point expiry;
    std::coroutine_handle<> handle;

    /**
     * @brief Compare events by expiry so the priority queue has the earliest
     * time point at the top.
     *
     * @param[in] other Event to compare with.
     * @return True when this event expires later than the other event.
     */
    bool operator>(const SampleSubscriptionEvent &other) const { return expiry > other.expiry; }
};

/**
 * @brief The scheduler multiplexes all subscription events of a single stream
 * subscription onto one worker thread. Timed SAMPLE events are coroutines
 * suspending in async_sleep and resumed at their expiry, ON_CHANGE
 * notifications are plain tasks queued by the sysrepo notification
 * callbacks. This way the per-subscription timers need no dedicated
 * threads, and only one thread writes to the gRPC stream (serialized
 * with the handler thread by Subscribe::Write).
 *
 */
class Scheduler
{
  private:
    // sample subscription is evoked after a set duration
    std::priority_queue<SampleSubscriptionEvent, std::vector<SampleSubscriptionEvent>,
        std::greater<SampleSubscriptionEvent>>
        sample_queue_;

    // on-change subscriptions are evoked immediately on change
    std::queue<std::function<void()>> change_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

  public:
    /**
     * @brief Queue a sample subscription event for a timed execution.
     *
     * @param[in] expiry Time point at which the coroutine is resumed.
     * @param[in] h Coroutine handle to resume.
     */
    void schedule_sample(std::chrono::steady_clock::time_point expiry, std::coroutine_handle<> h)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            sample_queue_.push({expiry, h});
        }
        cv_.notify_one(); // wake up subscription event queue
    }

    /**
     * @brief Queue an on-change subscription event for immediate execution.
     *
     * @param[in] task Task delivering the notification.
     */
    void schedule_change(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            change_queue_.push(std::move(task));
        }
        cv_.notify_one(); // wake up subscription event queue
    }

    /**
     * @brief Finish the scheduling.
     *
     */
    void stop()
    {
        running_ = false;
        cv_.notify_all(); // notify all subscription events and return to the caller
    }

    /**
     * @brief Run the scheduler loop until stopped.
     *
     */
    void run()
    {
        while (running_)
        {
            std::function<void()> change_task;
            std::coroutine_handle<> sample_task;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                // wait for: subscription event or stream to cancel
                cv_.wait(lock, [this]
                    { return !sample_queue_.empty() || !change_queue_.empty() || !running_; });

                // stream cancelled, finish work
                if (!running_)
                {
                    break;
                }

                // on-change (immediate) subscription events
                if (!change_queue_.empty())
                {
                    change_task = std::move(change_queue_.front());
                    change_queue_.pop();
                }
                // sample (timed) subscription events
                else if (!sample_queue_.empty())
                {
                    const SampleSubscriptionEvent &event = sample_queue_.top();
                    std::chrono::time_point now = std::chrono::steady_clock::now();

                    // wait for the timer to expire
                    if (event.expiry > now)
                    {
                        // cancel at a specified time point or handle on-change subscription or
                        // stream cancellation
                        cv_.wait_until(lock, event.expiry,
                            [this] { return !change_queue_.empty() || !running_; });

                        // on-change subscription interrupted, so resolve it!
                        if (!change_queue_.empty())
                        {
                            continue;
                        }

                        // stream cancelled, finish work
                        if (!running_)
                        {
                            break;
                        }
                    }

                    // dequeue the subscription event
                    sample_queue_.pop();
                    sample_task = event.handle;
                }
            } // unlocks mutex

            // run a specific task from the scheduler
            if (change_task)
            {
                change_task();
            }
            else if (sample_task)
            {
                sample_task.resume();
            }
        }

        // destroy pending subscription events
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!sample_queue_.empty())
        {
            auto event = sample_queue_.top();
            sample_queue_.pop();
            event.handle.destroy();
        }

        SLOG_DEBUG("Subscription Event Scheduler shutting down.");
    }
};

/**
 * @brief Coroutine awaitable suspending a sample subscription for its sample
 * interval.
 *
 */
struct AsyncSleep
{
    Scheduler &scheduler;
    std::chrono::nanoseconds duration;

    /**
     * @brief With duration less-than-or-equal to zero, do not suspend.
     *
     * @return True when the coroutine must not suspend.
     */
    bool await_ready() const noexcept { return duration.count() <= 0; }

    /**
     * @brief Schedule the wakeup at the expiry of the duration, called with
     * every co_await.
     *
     * @param[in] h Handle of the coroutine to resume.
     */
    void await_suspend(std::coroutine_handle<> h) const noexcept
    {
        auto expiry = std::chrono::steady_clock::now() + duration;
        scheduler.schedule_sample(expiry, h);
    }

    /**
     * @brief Resume the coroutine (nothing to do).
     *
     */
    void await_resume() const noexcept {}
};

/**
 * @brief Minimal coroutine task type used for sample subscriptions.
 *
 */
struct Task
{
    /**
     * @brief Promise of the sample-subscription coroutine: starts eagerly,
     * does not suspend at either end and terminates on exceptions.
     *
     */
    struct promise_type
    {
        /**
         * @brief Return the (empty) task object to the caller of the coroutine.
         *
         * @return Empty task object.
         */
        Task get_return_object() { return {}; }

        /**
         * @brief Do not suspend the coroutine at its start.
         *
         * @return The std::suspend_never awaitable.
         */
        std::suspend_never initial_suspend() noexcept { return {}; }

        /**
         * @brief Do not suspend the coroutine at its end.
         *
         * @return The std::suspend_never awaitable.
         */
        std::suspend_never final_suspend() noexcept { return {}; }

        /**
         * @brief Deliver the (void) result of the coroutine.
         *
         */
        void return_void() {}

        /**
         * @brief Terminate the process on an exception escaping the coroutine.
         *
         */
        void unhandled_exception() { std::terminate(); }
    };
};

/**
 * @brief Coroutine producing the periodic updates of a sample subscription.
 *
 * @param[in,out] scheduler Scheduler resuming the coroutine at each expiry.
 * @param[in] duration Sample interval.
 * @param[in] subscribe Subscribe implementation used to build the updates.
 * @param[in] context gRPC server context.
 * @param[in] sub Sample subscription to serve.
 * @param[in,out] stream Stream to write the updates to.
 * @param[in] encoding Encoding of the original subscription list.
 * @param[in] prefix Prefix of the original subscription list.
 * @return Task object of the coroutine.
 */
Task sample_subscription(Scheduler &scheduler, std::chrono::nanoseconds duration,
    Subscribe *subscribe, grpc::ServerContext *context, std::shared_ptr<gnmi::Subscription> sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    gnmi::Encoding encoding, gnmi::Path prefix)
{
    while (true)
    {
        // suspend, the scheduler will resume you after the time duration
        co_await AsyncSleep{scheduler, duration};
        subscribe->trigger_sample_update(context, sub, stream, encoding, prefix);
    }
}

void Subscribe::stream_worker(grpc::ServerContext *context, gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler)
{
    for (int i = 0; i < request.subscribe().subscription_size(); i++)
    {
        auto sub = std::make_shared<gnmi::Subscription>(request.subscribe().subscription(i));
        switch (sub->mode())
        {
        case gnmi::SAMPLE:
            sample_subscription(scheduler, std::chrono::nanoseconds{sub->sample_interval()}, this,
                context, sub, stream, request.subscribe().encoding(), request.subscribe().prefix());
            break;
        default:
            break;
        }
    }

    scheduler.run();

    SLOG_DEBUG("Subscription stream worker exiting");
}

/**
 * @brief Thread entry point running the stream worker.
 *
 * @param[in] sub Subscribe implementation the worker belongs to.
 * @param[in] context gRPC server context.
 * @param[in] request Subscription request the worker serves.
 * @param[in,out] stream Bidirectional stream of subscribe requests and responses.
 * @param[in,out] scheduler Scheduler multiplexing the subscription events.
 */
static void stream_worker_thread(Subscribe *sub, grpc::ServerContext *context,
    gnmi::SubscribeRequest &request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler)
{
    sub->stream_worker(context, request, stream, scheduler);
}

/**
 * @brief Parameters passed to the sysrepo module-change callback of an
 * on-change stream subscription.
 *
 */
class SrModuleOnChangeParams
{
  public:
    SrModuleOnChangeParams(gnmi::SubscribeRequest *request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Subscribe *subscribe, Scheduler &scheduler, std::string origin)
        : request(request), stream(stream), subscribe(subscribe), scheduler(scheduler),
          origin(std::move(origin))
    {
    }

    gnmi::SubscribeRequest *request;
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream;
    Subscribe *subscribe;
    Scheduler &scheduler;
    std::string origin;
};

/**
 * @brief Sysrepo module-change callback of an on-change subscription: build
 * a notification of the changes and queue it for delivery on the stream.
 *
 * @param[in] session Sysrepo session the changes are read from.
 * @param[in] module_name Name of the changed module (unused).
 * @param[in] xpath XPath the change subscription fires for.
 * @param[in] event Sysrepo event (unused).
 * @param[in] request_id Sysrepo request ID (unused).
 * @param[in] params Callback parameters of the subscription.
 * @return sysrepo::ErrorCode::Ok even on failures (only logged).
 */
sysrepo::ErrorCode sr_module_on_change(sysrepo::Session session, std::string_view module_name,
    std::string_view xpath, sysrepo::Event event, uint32_t request_id,
    const SrModuleOnChangeParams &params)
{
    grpc::Status status;
    auto response = std::make_shared<gnmi::SubscribeResponse>();

    (void)module_name;
    (void)event;
    (void)request_id;

    std::string changes_path(xpath);

    status = params.subscribe->build_subscribe_notification_for_changes(response->mutable_update(),
        params.request->subscribe(), changes_path, params.origin, session);
    if (!status.ok())
    {
        SLOG_WARN("unable to build update in response to notification for ", xpath);
        return sysrepo::ErrorCode::Ok;
    }

    auto sub = params.subscribe;
    auto stream = params.stream;

    params.scheduler.schedule_change([sub, stream, response] { sub->write(stream, *response); });

    return sysrepo::ErrorCode::Ok;
}

grpc::Status Subscribe::register_stream_on_change_(gnmi::SubscribeRequest &request,
    gnmi::Subscription sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler, std::shared_ptr<DataSubscribe> sr_sub,
    std::vector<SrModuleOnChangeParams> &params_vec)
{
    std::string origin;
    std::string fullpath;
    try
    {
        origin = gnxi::resolve_origin(request.subscribe().prefix(), sub.path());
        fullpath =
            gnxi::gnmi_to_xpath(sr_sess_.getContext(), request.subscribe().prefix(), sub.path());
    }
    catch (const grpc::Status &status)
    {
        SLOG_WARN("Invalid subscription prefix or path: ", status.error_message());
        return status;
    }

    SLOG_DEBUG("Subscribe (stream) ", fullpath);

    SrModuleOnChangeParams params(&request, stream, this, scheduler, origin);
    params_vec.push_back(params);
    try
    {
        SrModuleOnChangeParams &params_ref = params_vec.back();
        sr_sub->data_change_subscribe(
            [params_ref](sysrepo::Session session, uint32_t sub_id, std::string_view module_name,
                std::optional<std::string_view> xpath, sysrepo::Event event, uint32_t request_id)
            {
                (void)sub_id;
                return sr_module_on_change(
                    session, module_name, xpath.value(), event, request_id, params_ref);
            },
            fullpath.c_str(), 0,
            sysrepo::SubscribeOptions::Passive | sysrepo::SubscribeOptions::DoneOnly |
                sysrepo::SubscribeOptions::Enabled);
    }
    catch (const sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR(exc.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
    }

    return grpc::Status::OK;
}

/**
 * Handles SubscribeRequest messages with STREAM subscription mode by
 * periodically sending updates to the client.
 */
grpc::Status Subscribe::handle_stream_(grpc::ServerContext *context, gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    gnmi::SubscribeResponse response;
    grpc::Status status;
    std::vector<SrModuleOnChangeParams> params_vec;

    if (request.subscribe().subscription_size() == 0)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "No subscription in message");
    }

    for (int i = 0; i < request.subscribe().subscription_size(); i++)
    {
        gnmi::Subscription &sub = *request.mutable_subscribe()->mutable_subscription(i);

        // checks that sample_interval values are not higher than INT64_MAX
        // i.e. 9223372036854775807 nanoseconds
        if (sub.sample_interval() >
            static_cast<uint64_t>(std::chrono::duration<long long, std::nano>::max().count()))
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                std::string("sample_interval must be less than ") + std::to_string(INT64_MAX) +
                    " nanoseconds");
        }

        // gNMI spec: a sample_interval of 0 means the target MUST create
        // the subscription and send the data with the lowest interval
        // possible for the target - clamp it to our minimum
        if (sub.mode() == gnmi::SubscriptionMode::SAMPLE && sub.sample_interval() == 0)
        {
            SLOG_INFO("sample_interval of 0 requested, using the minimum of ",
                std::to_string(std::chrono::nanoseconds{MIN_SAMPLE_INTERVAL}.count()),
                " nanoseconds");
            sub.set_sample_interval(std::chrono::nanoseconds{MIN_SAMPLE_INTERVAL}.count());
        }

        if (sub.mode() == gnmi::SubscriptionMode::TARGET_DEFINED)
        {
            SLOG_WARN("TARGET_DEFINED subscription mode not supported");
            return grpc::Status(
                grpc::StatusCode::UNIMPLEMENTED, "TARGET_DEFINED subscription mode not supported");
        }

        if (sub.mode() == gnmi::SubscriptionMode::SAMPLE &&
            std::chrono::nanoseconds{sub.sample_interval()} < MIN_SAMPLE_INTERVAL)
        {
            SLOG_WARN("sample_interval ", std::to_string(sub.sample_interval()),
                " must be at least ",
                std::to_string(std::chrono::nanoseconds{MIN_SAMPLE_INTERVAL}.count()),
                " nanoseconds");
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                std::string("sample_interval ") + std::to_string(sub.sample_interval()) +
                    " must be at least " +
                    std::to_string(std::chrono::nanoseconds{MIN_SAMPLE_INTERVAL}.count()) +
                    " nanoseconds");
        }
    }

    // get the initial data only for sample subscriptions
    bool sample = false;
    status = build_subscribe_notification(response.mutable_update(), request.subscribe(), &sample);
    if (!status.ok())
    {
        return status;
    }

    Scheduler scheduler;

    // the on-change subscriptions are registered with sysrepo on this
    // session, which must be switched to the Operational datastore (running
    // configuration plus state data) so that gNMI streams expose both
    SessionDsSwitcher ds_switch(sr_sess_, sysrepo::Datastore::Operational);
    auto sr_sub = std::make_shared<DataSubscribe>(sr_sess_);

    if (sample)
    {
        SLOG_DEBUG("Sending initial update for sample subscriptions with size:",
            response.update().update_size());
        // sends a first Notification message that updates all sample subcriptions
        write(stream, response);
    }
    for (int i = 0; i < request.subscribe().subscription_size(); i++)
    {
        gnmi::Subscription sub = request.subscribe().subscription(i);
        switch (sub.mode())
        {
        case gnmi::SAMPLE:
            SLOG_DEBUG("Subscribe (stream sample) ", sub.path().ShortDebugString());
            break;
        case gnmi::ON_CHANGE:
            status =
                register_stream_on_change_(request, sub, stream, scheduler, sr_sub, params_vec);
            if (!status.ok())
            {
                return status;
            }
            break;
        default:
            SLOG_WARN("subscription mode ", std::to_string(sub.mode()), " not implemented");
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                std::string(
                    "subscription mode " + std::to_string(sub.mode()) + " not implemented"));
        }
    }

    SLOG_INFO(
        "STREAM subscription established, ", request.subscribe().subscription_size(), " paths");

    // send to the worker thread
    scheduler.schedule_change(
        [this, stream]
        {
            // sends a SYNC message that indicates that initial synchronization
            // has completed, i.e. each Subscription has been updated once
            gnmi::SubscribeResponse response;
            response.set_sync_response(true);
            SLOG_DEBUG("Sending sync response");
            write(stream, response);
        });

    // start a worker thread for SAMPLE and ON_CHANGE notifications (the
    // only modes left - TARGET_DEFINED is rejected above)
    std::thread thread = std::thread(
        stream_worker_thread, this, context, std::ref(request), stream, std::ref(scheduler));

    try
    {
        // read from client - note that isn't expected to succeed, but allows us to
        // wait (without a busy loop) until the client cancels the streaming subscription and
        // then we can terminate the worker thread immediately
        gnmi::SubscribeRequest request2;
        bool success = stream->Read(&request2);

        scheduler.stop();
        thread.join();

        if (success)
        {
            SLOG_WARN("out-of-order operation was requested on a STREAM subscription");
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                std::string("out-of-order operation was requested on a STREAM subscription"));
        }

        return grpc::Status::OK;
    }
    catch (...)
    {
        // make sure the worker thread is always terminated, the central
        // RPC exception handler in gnmi.cpp takes care of the rest
        scheduler.stop();
        thread.join();
        throw;
    }
}

void Subscribe::write(
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    gnmi::SubscribeResponse response)
{
    const std::lock_guard<std::recursive_mutex> lock(stream_mutex_);
    stream->Write(response);
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating
 * all the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
grpc::Status Subscribe::handle_once_(gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    grpc::Status status;

    // sends a Notification message that updates all Subcriptions once
    gnmi::SubscribeResponse response;
    status = build_subscribe_notification(response.mutable_update(), request.subscribe());
    if (!status.ok())
    {
        return status;
    }

    write(stream, response);
    response.Clear();

    // sends a message that indicates that initial synchronization
    // has completed, i.e. each Subscription has been updated once
    response.set_sync_response(true);
    write(stream, response);
    response.Clear();

    return grpc::Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating
 * all the Subscriptions each time a Poll request is received.
 */
grpc::Status Subscribe::handle_poll_(gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    gnmi::SubscribeRequest subscription = request;
    grpc::Status status;

    while (stream->Read(&request))
    {
        switch (request.request_case())
        {
        case request.kPoll:
        {
            // sends a Notification message that updates all Subcriptions once
            gnmi::SubscribeResponse response;
            status =
                build_subscribe_notification(response.mutable_update(), subscription.subscribe());
            if (!status.ok())
            {
                return status;
            }
            write(stream, response);
            response.Clear();

            // gNMI reference 3.5.2.3:
            // "For POLL subscriptions, after each set of updates for individual poll request, a
            // SubscribeResponse message with the sync_response field set to true MUST be
            // generated."
            response.set_sync_response(true);
            write(stream, response);
            break;
        }
        case request.kSubscribe:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                "A SubscriptionList has already been received for this RPC");
        default:
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT, "Unknown content for SubscribeRequest message");
        }
    }

    return grpc::Status::OK;
}

/**
 * Handles the first SubscribeRequest message.
 * If it does not have the "subscribe" field set, the RPC MUST be cancelled.
 * Ref: 3.5.1.1
 */
grpc::Status Subscribe::run(grpc::ServerContext *context,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    gnmi::SubscribeRequest request;

    stream->Read(&request);

    if (request.extension_size() > 0)
    {
        SLOG_WARN("Extensions not implemented");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Extensions not implemented");
    }

    if (!request.has_subscribe())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
            "SubscribeRequest needs non-empty SubscriptionList");
    }

    // advisory gNMI fields that are accepted but currently ignored - the
    // stream works correctly without them
    if (request.subscribe().allow_aggregation())
    {
        SLOG_WARN("allow_aggregation unsupported, updates are not aggregated");
    }
    if (request.subscribe().has_qos())
    {
        SLOG_WARN("qos marking unsupported, ignored");
    }
    for (const auto &s : request.subscribe().subscription())
    {
        if (s.suppress_redundant())
        {
            SLOG_WARN("suppress_redundant unsupported, redundant updates are sent");
        }
        if (s.heartbeat_interval())
        {
            SLOG_WARN("heartbeat_interval unsupported, no heartbeat notifications are sent");
        }
    }

    switch (request.subscribe().mode())
    {
    case gnmi::SubscriptionList_Mode_STREAM:
        return handle_stream_(context, request, stream);
    case gnmi::SubscriptionList_Mode_ONCE:
        return handle_once_(request, stream);
    case gnmi::SubscriptionList_Mode_POLL:
        return handle_poll_(request, stream);
    default:
        SLOG_WARN("Unknown subscription mode");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unknown subscription mode");
    }
}

} // namespace impl
