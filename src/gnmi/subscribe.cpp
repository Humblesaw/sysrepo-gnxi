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

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <proto/gnmi.grpc.pb.h>
// #include <grpcpp/impl/codegen/core_codegen_interface.h>

#include "subscribe.h"
#include "utils/sysrepo.h"
#include <sysrepo-cpp/Changes.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <utils/log.h>
#include <utils/utils.h>

namespace impl
{

grpc::Status
Subscribe::BuildSubsUpdate(google::protobuf::RepeatedPtrField<gnmi::Update> *updateList,
                           const gnmi::Path &prefix, std::string fullpath, gnmi::Encoding encoding)
{
    gnmi::Update *update;

    if (prefix.elem_size() > 0)
    {
        std::string str = gnmi_to_xpath(prefix);
        fullpath = str + fullpath;
    }

    SessionDsSwitcher ds_switch(sr_sess, sysrepo::Datastore::Operational);

    try
    {
        /* Get multiple subtree for YANG lists or one for other YANG types */
        std::optional<libyang::DataNode> sr_trees = sr_sess.getData(fullpath.c_str());

        /* The path not (yet) existing isn't an error, so just return an empty set */
        if (!sr_trees.has_value())
        {
            return grpc::Status::OK;
        }

        for (libyang::DataNode n : sr_trees->findXPath(fullpath.c_str()))
        {
            update = updateList->Add();
            xpath_to_gnmi(n.path(), *update->mutable_path());
            grpc::Status status = encodef->encode(encoding, n, update->mutable_val());
            if (!status.ok())
            {
                updateList->Clear();
                return status;
            }
        }
    }
    catch (std::invalid_argument &exc)
    {
        updateList->Clear();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, exc.what());
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        updateList->Clear();
        SLOG_ERROR("Fail getting items from sysrepo: ", exc.code());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }

    return grpc::Status::OK;
}

/**
 * BuildSubscribeNotification - Build a Notification message, excluding
 * subscriptions which are on-change(this is done elsewhere, racy if done here).
 * Contrary to Get Notification, gnmi specification highly recommends to
 * put multiple <xpath, value> in the same Notification message.
 * @param notification the notification that is constructed by this function.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 * @param sample indicates whether there is at least 1 sample subscr
 */
grpc::Status Subscribe::BuildSubscribeNotification(gnmi::Notification *notification,
                                                   const gnmi::SubscriptionList &request,
                                                   bool *sample)
{
    google::protobuf::RepeatedPtrField<gnmi::Update> *updateList = notification->mutable_update();
    grpc::Status status;

    /* Check if only updates should be sent */
    if (request.updates_only())
    {
        SLOG_WARN("Unsupported updates_only, send all paths");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "updates-only not supported");
    }

    /* Get time since epoch in milliseconds */
    notification->set_timestamp(get_time_nanosec());

    // gNMI spec §2.2.2.1:
    // When set in the prefix in a request, GetRequest, SetRequest or
    // SubscribeRequest, the field MUST be reflected in the prefix of the
    // corresponding GetResponse, SetResponse or SubscribeResponse by a
    // server.
    if (request.has_prefix())
    {
        notification->mutable_prefix()->set_target(request.prefix().target());
    }

    if (sample)
    {
        *sample = false;
    }

    /* Fill Update RepeatedPtrField in Notification message
     * Update field contains only data elements that have changed values. */
    for (int i = 0; i < request.subscription_size(); i++)
    {
        gnmi::Subscription sub = request.subscription(i);

        if (request.mode() == gnmi::SubscriptionList_Mode_STREAM &&
            (sub.mode() == gnmi::SubscriptionMode::TARGET_DEFINED ||
             sub.mode() == gnmi::SubscriptionMode::ON_CHANGE))
        {
            SLOG_DEBUG("On-change, getting initial data later: ", gnmi_to_xpath(sub.path()));
            continue;
        }
        if (sample)
        {
            *sample = true;
        }

        // Fetch all found counters value for a requested path
        std::string str;
        try
        {
            gnmi_check_origin(request.prefix(), sub.path());

            status = BuildSubsUpdate(updateList, request.prefix(), gnmi_to_xpath(sub.path()),
                                     request.encoding());
        }
        catch (std::invalid_argument &exc)
        {
            SLOG_ERROR(exc.what());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
        }
        if (!status.ok())
        {
            SLOG_ERROR("Fail building update for ", gnmi_to_xpath(sub.path()));
            return status;
        }
    }

    notification->set_atomic(false);

    return grpc::Status::OK;
}

/**
 * BuildSubscribeNotificationForChanges - Build a Notification message.
 * @param notification the notification that is constructed by this function.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 * @param xpath The xpath that the registration is firing for
 * @param session The sysrepo session for the update
 */
grpc::Status Subscribe::BuildSubscribeNotificationForChanges(gnmi::Notification *notification,
                                                             const gnmi::SubscriptionList &request,
                                                             std::string &xpath,
                                                             sysrepo::Session session)
{
    auto updateList = notification->mutable_update();
    auto deleteList = notification->mutable_delete_();
    grpc::Status status;

    /* Check if only updates should be sent */
    if (request.updates_only())
    {
        SLOG_WARN("Unsupported updates_only, send all paths");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "updates-only not supported");
    }

    /* Get time since epoch in milliseconds */
    notification->set_timestamp(get_time_nanosec());

    // gNMI spec §2.2.2.1:
    // When set in the prefix in a request, GetRequest, SetRequest or
    // SubscribeRequest, the field MUST be reflected in the prefix of the
    // corresponding GetResponse, SetResponse or SubscribeResponse by a
    // server.
    if (request.has_prefix())
        notification->mutable_prefix()->set_target(request.prefix().target());

    /* Fill Update RepeatedPtrField in Notification message
     * Update field contains only data elements that have changed values. */

    try
    {
        auto last_change = make_pair(std::string(""), sysrepo::ChangeOperation::Created);

        std::string changes_path(xpath);
        changes_path += "//.";
        auto iter = session.getChanges(changes_path.c_str());
        for (const auto &change : iter)
        {
            if (!last_change.first.empty() && last_change.second == change.operation &&
                // If we have the identifier of one leaf as a substring of another at the same
                // level, we can confuse between the two. for example searching for "ike-connection"
                // and finding "ike-connection-up". So search with "/" suffixed and prevent this
                // mix-up.
                (change.node.path().rfind(last_change.first + "/", 0) == 0 ||
                 change.node.path() == last_change.first))
            {
                continue;
            }
            last_change = std::make_pair(change.node.path(), change.operation);

            auto val =
                change.node.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Siblings)
                    .value();
            SLOG_DEBUG("Subscribe notification, operation: ", change.operation,
                       ", path: ", change.node.path(), ", value: ", val);

            // Also done for updated nodes due to gNMI spec §3.5.2.3:
            // > To replace the contents of an entire node within the tree, the target populates
            // > the delete field with the path of the node being removed, along with the new
            // > contents within the update field.
            if (change.operation != sysrepo::ChangeOperation::Created)
            {
                auto path_p = deleteList->Add();
                gnmi::Path path;
                xpath_to_gnmi(change.node.path(), path);
                *path_p = path;
            }
            if (change.operation != sysrepo::ChangeOperation::Deleted)
            {
                auto update = updateList->Add();

                xpath_to_gnmi(change.node.path(), *update->mutable_path());
                // Remove all of the attributes from nodes which we don't need and may confuse
                // parsers of the JSON when using that encoding.
                // auto opts = static_cast<uint32_t>(libyang::DuplicationOptions::NoMeta) |
                //             static_cast<uint32_t>(libyang::DuplicationOptions::Recursive);
                // auto node =
                // change.node.duplicate(static_cast<libyang::DuplicationOptions>(opts));
                status = encodef->encode(request.encoding(), change.node, update->mutable_val());
                if (!status.ok())
                    return status;
            }
        }
    }
    catch (sysrepo::ErrorWithCode &exc)
    {
        SLOG_ERROR("Fail processing module changes from sysrepo: ", exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    catch (std::invalid_argument &exc)
    {
        SLOG_ERROR(exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }

    notification->set_atomic(false);

    return grpc::Status::OK;
}

void Subscribe::triggerSampleUpdate(
    grpc::ServerContext *context, std::shared_ptr<gnmi::Subscription> &sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    gnmi::SubscribeResponse response;
    gnmi::SubscriptionList updateList;

    // Add the subscription entry to the subscription list
    updateList.add_subscription()->CopyFrom(*sub);
    // gnmi::Path *prefix = new gnmi::Path();
    // prefix->set_origin("rfc7951");
    // updateList.set_allocated_prefix(prefix);

    if (!context->IsCancelled())
    {
        grpc::Status status = BuildSubscribeNotification(response.mutable_update(), updateList);
        if (!status.ok())
        {
            // This is a hack to allow the Read in the parent thread to return,
            // but it avoids needing to move to an asynchronous model just to return this one error
            // grpc::g_core_codegen_interface->grpc_call_cancel_with_status(
            //   context->c_call(), static_cast<grpc_status_code>(status.error_code()),
            //   status.error_message().c_str(), nullptr);
            return;
        }
        Write(stream, response);
        response.Clear();
    }
}

struct SampleSubscriptionEvent
{
    std::chrono::steady_clock::time_point expiry;
    std::coroutine_handle<> handle;

    // the priority queue has earliest time point at the top
    bool operator>(const SampleSubscriptionEvent &other) const { return expiry > other.expiry; }
};

class Scheduler
{
  private:
    /* sample subscription is evoked after a set duration */
    std::priority_queue<SampleSubscriptionEvent, std::vector<SampleSubscriptionEvent>,
                        std::greater<SampleSubscriptionEvent>>
        sample_queue;

    /* on-change subscriptions are evoked immediately on change */
    std::queue<std::function<void()>> change_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> running{true};

  public:
    // queue a sample subscription event for a timed execution
    void schedule_sample(std::chrono::steady_clock::time_point expiry, std::coroutine_handle<> h)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            sample_queue.push({expiry, h});
        }
        cv.notify_one(); // wake up subscription event queue
    }

    // queue an on-change subscription event for immediate execution
    void schedule_change(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            change_queue.push(std::move(task));
        }
        cv.notify_one(); // wake up subscription event queue
    }

    // finish the scheduling
    void stop()
    {
        running = false;
        cv.notify_all(); // notify all subscription events and return to the caller
    }

    void run()
    {
        while (running)
        {
            std::function<void()> change_task;
            std::coroutine_handle<> sample_task;

            {
                std::unique_lock<std::mutex> lock(queue_mutex);

                // wait for: subscription event or stream to cancel
                cv.wait(lock, [this]
                        { return !sample_queue.empty() || !change_queue.empty() || !running; });

                // stream cancelled, finish work
                if (!running)
                {
                    break;
                }

                // on-change (immediate) subscription events
                if (!change_queue.empty())
                {
                    change_task = std::move(change_queue.front());
                    change_queue.pop();
                }
                // sample (timed) subscription events
                else if (!sample_queue.empty())
                {
                    const SampleSubscriptionEvent &event = sample_queue.top();
                    std::chrono::time_point now = std::chrono::steady_clock::now();

                    // wait for the timer to expire
                    if (event.expiry > now)
                    {
                        // cancel at a specified time point or handle on-change subscription or
                        // stream cancellation
                        cv.wait_until(lock, event.expiry,
                                      [this] { return !change_queue.empty() || !running; });

                        // on-change subscription interrupted, so resolve it!
                        if (!change_queue.empty())
                        {
                            continue;
                        }

                        // stream cancelled, finish work
                        if (!running)
                        {
                            break;
                        }
                    }

                    // dequeue the subscription event
                    sample_queue.pop();
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
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!sample_queue.empty())
        {
            auto event = sample_queue.top();
            sample_queue.pop();
            event.handle.destroy();
        }

        SLOG_DEBUG("Subscription Event Scheduler shutting down.");
    }
};

struct async_sleep
{
    Scheduler &scheduler;
    std::chrono::nanoseconds duration;

    // with duration less-or-equal to zero, do not suspend
    bool await_ready() const noexcept { return duration.count() <= 0; }

    // called with every co_await
    void await_suspend(std::coroutine_handle<> h) const noexcept
    {
        auto expiry = std::chrono::steady_clock::now() + duration;
        scheduler.schedule_sample(expiry, h);
    }

    void await_resume() const noexcept {}
};

struct Task
{
    struct promise_type
    {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

Task sampleSubscription(
    Scheduler &scheduler, std::chrono::nanoseconds duration, Subscribe *subscribe,
    grpc::ServerContext *context, std::shared_ptr<gnmi::Subscription> sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    while (true)
    {
        // suspend, the scheduler will resume you after the time duration
        co_await async_sleep{scheduler, duration};
        subscribe->triggerSampleUpdate(context, sub, stream);
    }
}

void Subscribe::streamWorker(
    grpc::ServerContext *context, gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler)
{
    for (int i = 0; i < request.subscribe().subscription_size(); i++)
    {
        auto sub = std::make_shared<gnmi::Subscription>(request.subscribe().subscription(i));
        switch (sub->mode())
        {
        case gnmi::SAMPLE:
            sampleSubscription(scheduler, std::chrono::nanoseconds{sub->sample_interval()}, this,
                               context, sub, stream);
            break;
        default:
            break;
        }
    }

    scheduler.run();

    SLOG_DEBUG("Subscription stream worker exiting");
}

static void streamWorkerThread(
    Subscribe *sub, grpc::ServerContext *context, gnmi::SubscribeRequest &request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler)
{
    sub->streamWorker(context, request, stream, scheduler);
}

class SrModuleOnChangeParams
{
  public:
    SrModuleOnChangeParams(
        gnmi::SubscribeRequest *request,
        grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
        Subscribe *subscribe, Scheduler &scheduler)
        : request(request), stream(stream), subscribe(subscribe), scheduler(scheduler)
    {
    }

    gnmi::SubscribeRequest *request;
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream;
    Subscribe *subscribe;
    Scheduler &scheduler;
};

sysrepo::ErrorCode srModuleOnChange(sysrepo::Session session, std::string_view module_name,
                                    std::string_view xpath, sysrepo::Event event,
                                    uint32_t request_id, const SrModuleOnChangeParams &params)
{
    grpc::Status status;
    auto response = std::make_shared<gnmi::SubscribeResponse>();

    (void)module_name;
    (void)event;
    (void)request_id;

    std::string changes_path(xpath);

    status = params.subscribe->BuildSubscribeNotificationForChanges(
        response->mutable_update(), params.request->subscribe(), changes_path, session);
    if (!status.ok())
    {
        SLOG_WARN("unable to build update in response to notification for ", xpath);
        return sysrepo::ErrorCode::Ok;
    }

    auto sub = params.subscribe;
    auto stream = params.stream;

    params.scheduler.schedule_change([sub, stream, response] { sub->Write(stream, *response); });

    return sysrepo::ErrorCode::Ok;
}

grpc::Status Subscribe::registerStreamOnChange(
    gnmi::SubscribeRequest &request, gnmi::Subscription sub,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    Scheduler &scheduler, std::shared_ptr<DataSubscribe> sr_sub,
    std::vector<SrModuleOnChangeParams> &params_vec)
{
    std::string fullpath = "";
    try
    {
        if (request.subscribe().prefix().elem_size() > 0 ||
            request.subscribe().prefix().target().compare(""))
        {
            fullpath = gnmi_to_xpath(request.subscribe().prefix());
        }
        fullpath += gnmi_to_xpath(sub.path());
    }
    catch (std::invalid_argument &exc)
    {
        SLOG_ERROR(exc.what());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }

    SLOG_DEBUG("Subscribe (stream) ", fullpath);

    SrModuleOnChangeParams params(&request, stream, this, scheduler);
    params_vec.push_back(params);
    try
    {
        SrModuleOnChangeParams &params_ref = params_vec.back();
        sr_sub->data_change_subscribe(
            [params_ref](sysrepo::Session session, uint32_t sub_id, std::string_view module_name,
                         std::optional<std::string_view> xpath, sysrepo::Event event,
                         uint32_t request_id)
            {
                (void)sub_id;
                return srModuleOnChange(session, module_name, xpath.value(), event, request_id,
                                        params_ref);
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
grpc::Status Subscribe::handleStream(
    grpc::ServerContext *context, gnmi::SubscribeRequest request,
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
        gnmi::Subscription sub = request.subscribe().subscription(i);

        // Checks that sample_interval values are not higher than INT64_MAX
        // i.e. 9223372036854775807 nanoseconds
        if (sub.sample_interval() >
            static_cast<uint64_t>(std::chrono::duration<long long, std::nano>::max().count()))
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                std::string("sample_interval must be less than ") +
                                    std::to_string(INT64_MAX) + " nanoseconds");
        }

        if (sub.mode() == gnmi::SubscriptionMode::SAMPLE &&
            std::chrono::nanoseconds{sub.sample_interval()} < std::chrono::milliseconds(200))
        {
            SLOG_WARN(
                "sample_interval ", std::to_string(sub.sample_interval()), " must be greater than ",
                std::to_string(std::chrono::nanoseconds{std::chrono::milliseconds(200)}.count()),
                " nanoseconds");
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                std::string("sample_interval ") + std::to_string(sub.sample_interval()) +
                    " must be greater than " +
                    std::to_string(
                        std::chrono::nanoseconds{std::chrono::milliseconds(200)}.count()) +
                    " nanoseconds");
        }
    }

    // Get the initial data only for sample subscriptions
    bool sample = false;
    status = BuildSubscribeNotification(response.mutable_update(), request.subscribe(), &sample);
    if (!status.ok())
    {
        return status;
    }

    Scheduler scheduler;

    SessionDsSwitcher ds_switch(sr_sess, sysrepo::Datastore::Operational);
    auto sr_sub = std::make_shared<DataSubscribe>(sr_sess);

    if (sample)
    {
        SLOG_DEBUG("Sending initial update for sample subscriptions with size:",
                   response.update().update_size());
        // Sends a first Notification message that updates all sample subcriptions
        Write(stream, response);
    }
    for (int i = 0; i < request.subscribe().subscription_size(); i++)
    {
        gnmi::Subscription sub = request.subscribe().subscription(i);
        switch (sub.mode())
        {
        case gnmi::SAMPLE:
            SLOG_DEBUG("Subscribe (stream sample) ", gnmi_to_xpath(sub.path()));
            break;
        case gnmi::TARGET_DEFINED:
        case gnmi::ON_CHANGE:
            status = registerStreamOnChange(request, sub, stream, scheduler, sr_sub, params_vec);
            if (!status.ok())
            {
                return status;
            }
            break;
        default:
            SLOG_WARN("subscription mode ", std::to_string(sub.mode()), " not implemented");
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                std::string("subscription mode " + std::to_string(sub.mode()) +
                                            " not implemented"));
        }
    }

    // Send to the worker thread
    scheduler.schedule_change(
        [this, stream]
        {
            // Sends a SYNC message that indicates that initial synchronization
            // has completed, i.e. each Subscription has been updated once
            gnmi::SubscribeResponse response;
            response.set_sync_response(true);
            SLOG_DEBUG("Sending sync response");
            Write(stream, response);
        });

    // Start a worker thread for SAMPLE and ON_CHANGE notifications (the only other type,
    // TARGET_DEFINED, isn't supported).
    std::thread thread = std::thread(streamWorkerThread, this, context, std::ref(request), stream,
                                     std::ref(scheduler));

    // Read from client - note that isn't expected to succeed, but allows us to
    // wait (without a busy loop) until the client cancels the streaming subscription and
    // then we can terminate the worker thread immediately
    gnmi::SubscribeRequest request2;
    bool success = stream->Read(&request2);

    scheduler.stop();
    thread.join();

    if (success)
    {
        SLOG_WARN("out-of-order operation was requested on a STREAM subscription");
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            std::string("out-of-order operation was requested on a STREAM subscription"));
    }

    return grpc::Status::OK;
}

void Subscribe::Write(
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream,
    gnmi::SubscribeResponse response)
{
    const std::lock_guard<std::recursive_mutex> lock(stream_mutex);
    stream->Write(response);
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating
 * all the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
grpc::Status Subscribe::handleOnce(
    gnmi::SubscribeRequest request,
    grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    grpc::Status status;

    // Sends a Notification message that updates all Subcriptions once
    gnmi::SubscribeResponse response;
    status = BuildSubscribeNotification(response.mutable_update(), request.subscribe());
    if (!status.ok())
    {
        return status;
    }

    Write(stream, response);
    response.Clear();

    // Sends a message that indicates that initial synchronization
    // has completed, i.e. each Subscription has been updated once
    response.set_sync_response(true);
    Write(stream, response);
    response.Clear();

    return grpc::Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating
 * all the Subscriptions each time a Poll request is received.
 */
grpc::Status Subscribe::handlePoll(
    gnmi::SubscribeRequest request,
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
            // Sends a Notification message that updates all Subcriptions once
            gnmi::SubscribeResponse response;
            status =
                BuildSubscribeNotification(response.mutable_update(), subscription.subscribe());
            if (!status.ok())
            {
                return status;
            }
            Write(stream, response);
            response.Clear();

            // Reference 3.5.2.3:
            // "For POLL subscriptions, after each set of updates for individual poll request, a
            // SubscribeResponse message with the sync_response field set to true MUST be
            // generated."
            response.set_sync_response(true);
            Write(stream, response);
            break;
        }
        case request.kSubscribe:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "A SubscriptionList has already been received for this RPC");
        default:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "Unknown content for SubscribeRequest message");
        }
    }

    return grpc::Status::OK;
}

/**
 * Handles the first SubscribeRequest message.
 * If it does not have the "subscribe" field set, the RPC MUST be cancelled.
 * Ref: 3.5.1.1
 */
grpc::Status
Subscribe::run(grpc::ServerContext *context,
               grpc::ServerReaderWriter<gnmi::SubscribeResponse, gnmi::SubscribeRequest> *stream)
{
    gnmi::SubscribeRequest request;

    stream->Read(&request);

    if (request.extension_size() > 0)
    {
        SLOG_ERROR("Extensions not implemented");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Extensions not implemented");
    }

    if (!request.has_subscribe())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "SubscribeRequest needs non-empty SubscriptionList");
    }

    switch (request.subscribe().mode())
    {
    case gnmi::SubscriptionList_Mode_STREAM:
        return handleStream(context, request, stream);
    case gnmi::SubscriptionList_Mode_ONCE:
        return handleOnce(request, stream);
    case gnmi::SubscriptionList_Mode_POLL:
        return handlePoll(request, stream);
    default:
        SLOG_ERROR("Unknown subscription mode");
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unknown subscription mode");
    }

    return grpc::Status::OK;
}

} // namespace impl
