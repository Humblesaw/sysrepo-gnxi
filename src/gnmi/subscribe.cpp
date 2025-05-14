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

#include <memory>
#include <thread>
#include <chrono>
#include <string>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

#include <grpc/grpc.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>

#include <sysrepo-cpp/Changes.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "subscribe.h"
#include <utils/utils.h>
#include <utils/log.h>
#include "utils/sysrepo.h"

using namespace std;
using namespace chrono;
using google::protobuf::RepeatedPtrField;

namespace impl {

Status
Subscribe::BuildSubsUpdate(RepeatedPtrField<Update>* updateList,
                           const Path &prefix, string fullpath,
                           gnmi::Encoding encoding)
{
  Update *update;

  if (prefix.elem_size() > 0) {
    string str = gnmi_to_xpath(prefix);
    fullpath = str + fullpath;
  }

  SessionDsSwitcher ds_switch(sr_sess, sysrepo::Datastore::Operational);

  try {
    /* Get multiple subtree for YANG lists or one for other YANG types */
    auto sr_trees = sr_sess.getData(fullpath.c_str());
    /* The path not (yet) existing isn't an error, so just return an empty set */
    if (!sr_trees.has_value())
      return Status::OK;

    for (auto n : sr_trees->findXPath(fullpath.c_str())) {
      update = updateList->Add();
      xpath_to_gnmi(n.path(), *update->mutable_path());
      auto status = encodef->encode(encoding, n, update->mutable_val());
      if (!status.ok()) {
        updateList->Clear();
        return status;
      }
    }
  } catch (invalid_argument &exc) {
    updateList->Clear();
    return Status(StatusCode::NOT_FOUND, exc.what());
  } catch (sysrepo::ErrorWithCode &exc) {
    BOOST_LOG_TRIVIAL(error) << "Fail getting items from sysrepo: "
                              << exc.code();
    updateList->Clear();
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  }

  return Status::OK;
}

/**
 * BuildSubscribeNotification - Build a Notification message, excluding
 * subscriptions which are on-change(this is done elsewhere, racy if done here).
 * Contrary to Get Notification, gnmi specification highly recommands to
 * put multiple <xpath, value> in the same Notification message.
 * @param notification the notification that is constructed by this function.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 * @param sample indicates whether there is at least 1 sample subscr
 */
Status
Subscribe::BuildSubscribeNotification(Notification *notification,
                                      const SubscriptionList& request,
				      bool *sample)
{
  RepeatedPtrField<Update>* updateList = notification->mutable_update();
  Status status;

  // Defined refer to a long Path by a shorter one: alias
  if (request.use_aliases()) {
    BOOST_LOG_TRIVIAL(warning) << "Unsupported usage of aliases";
    return Status(StatusCode::UNIMPLEMENTED, "alias not supported");
  }

  /* Check if only updates should be sent */
  if (request.updates_only()) {
    BOOST_LOG_TRIVIAL(warning) << "Unsupported updates_only, send all paths";
    return Status(StatusCode::UNIMPLEMENTED, "updates-only not supported");
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

  if (sample) {
    *sample = false;
  }
  /* Fill Update RepeatedPtrField in Notification message
   * Update field contains only data elements that have changed values. */
  for (int i = 0; i < request.subscription_size(); i++) {
    Subscription sub = request.subscription(i);

    if (request.mode() == SubscriptionList_Mode_STREAM &&
	(sub.mode() == SubscriptionMode::TARGET_DEFINED ||
	 sub.mode() == SubscriptionMode::ON_CHANGE)) {
      BOOST_LOG_TRIVIAL(debug) << "On-change, getting initial data later: " << gnmi_to_xpath(sub.path());
      continue;
    }
    if (sample) {
      *sample = true;
    }
    // Fetch all found counters value for a requested path
    string str;
    try {
      gnmi_check_origin(request.prefix(), sub.path());

      status = BuildSubsUpdate(updateList, request.prefix(),
                               gnmi_to_xpath(sub.path()), request.encoding());
    } catch (invalid_argument &exc) {
      BOOST_LOG_TRIVIAL(error) << exc.what();
      return Status(StatusCode::INVALID_ARGUMENT, exc.what());
    }
    if (!status.ok()) {
      BOOST_LOG_TRIVIAL(error) << "Fail building update for "
                               << gnmi_to_xpath(sub.path());
      return status;
    }
  }

  notification->set_atomic(false);

  return Status::OK;
}

/**
 * BuildSubscribeNotificationForChanges - Build a Notification message.
 * @param notification the notification that is constructed by this function.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 * @param xpath The xpath that the registration is firing for
 * @param session The sysrepo session for the update
 */
Status
Subscribe::BuildSubscribeNotificationForChanges(Notification *notification,
                                                const SubscriptionList& request,
                                                string& xpath,
                                                sysrepo::Session session)
{
  auto updateList = notification->mutable_update();
  auto deleteList = notification->mutable_delete_();
  Status status;

  // Defined refer to a long Path by a shorter one: alias
  if (request.use_aliases()) {
    BOOST_LOG_TRIVIAL(warning) << "Unsupported usage of aliases";
    return Status(StatusCode::UNIMPLEMENTED, "alias not supported");
  }

  /* Check if only updates should be sent */
  if (request.updates_only()) {
    BOOST_LOG_TRIVIAL(warning) << "Unsupported updates_only, send all paths";
    return Status(StatusCode::UNIMPLEMENTED, "updates-only not supported");
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

  try {
    auto last_change = make_pair(std::string(""), sysrepo::ChangeOperation::Created);

    string changes_path(xpath);
    changes_path += "//.";
    auto iter = session.getChanges(changes_path.c_str());
    for (const auto& change : iter) {
      if (!last_change.first.empty() &&
          last_change.second == change.operation &&
          // If we have the identifier of one leaf as a substring of another at the same level,
          // we can confuse between the two.
          // for example searching for "ike-connection" and finding "ike-connection-up".
          // So search with "/" suffixed and prevent this mix-up.
          (change.node.path().rfind(last_change.first + "/", 0) == 0 ||
           change.node.path() == last_change.first)
         ) {
        continue;
      }
      last_change = make_pair(change.node.path(), change.operation);

      auto val = change.node.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings).value();
      BOOST_LOG_TRIVIAL(debug) << "Subscribe notification, operation: " << change.operation
          << ", path: " << change.node.path() << ", value: " << val;

      // Also done for updated nodes due to gNMI spec §3.5.2.3:
      // > To replace the contents of an entire node within the tree, the target populates
      // > the delete field with the path of the node being removed, along with the new
      // > contents within the update field.
      if (change.operation != sysrepo::ChangeOperation::Created) {
        auto path_p = deleteList->Add();
        Path path;
        xpath_to_gnmi(change.node.path(), path);
        *path_p = path;
      }
      if (change.operation != sysrepo::ChangeOperation::Deleted) {
        auto update = updateList->Add();

        xpath_to_gnmi(change.node.path(), *update->mutable_path());
        // Remove all of the attributes from nodes which we don't need and may confuse parsers of the JSON when
        // using that encoding.
        auto opts = static_cast<uint32_t>(libyang::DuplicationOptions::NoMeta) |
                        static_cast<uint32_t>(libyang::DuplicationOptions::Recursive);
        auto node = change.node.duplicate(static_cast<libyang::DuplicationOptions>(opts));
        status = encodef->encode(request.encoding(), node, update->mutable_val());
        if (!status.ok())
          return status;
      }
    }
  } catch (sysrepo::ErrorWithCode &exc) {
    BOOST_LOG_TRIVIAL(error) << "Fail processing module changes from sysrepo: "
                              << exc.what();
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  } catch (invalid_argument &exc) {
    BOOST_LOG_TRIVIAL(error) << exc.what();
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  }

  notification->set_atomic(false);

  return Status::OK;
}

void Subscribe::triggerSampleUpdate(
    ServerContext* context, Subscription &sub,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeResponse response;
  SubscriptionList updateList;

  // Add the subscription entry to the subscription list
  updateList.add_subscription()->CopyFrom(sub);

  if (!context->IsCancelled()) {
    auto status = BuildSubscribeNotification(response.mutable_update(),
                                             updateList);
    if(!status.ok()) {
      // This is a hack to allow the Read in the parent thread to return,
      // but it avoids needing to move to an asynchronous model just to return this one error
      grpc::g_core_codegen_interface->grpc_call_cancel_with_status(
        context->c_call(), static_cast<grpc_status_code>(status.error_code()),
        status.error_message().c_str(), nullptr);
      return;
    }
    Write(stream, response);
    response.Clear();
  }
}

static void sample_timer_expiry(
    const boost::system::error_code &e, std::shared_ptr<boost::asio::steady_timer> t,
    Subscription &sub, Subscribe *subscribe, ServerContext* context,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  if (e == boost::asio::error::operation_aborted) {
    return;
  }

  subscribe->triggerSampleUpdate(context, sub, stream);

  t->expires_at(t->expiry() + nanoseconds{sub.sample_interval()});
  t->async_wait(boost::bind(sample_timer_expiry,
        boost::asio::placeholders::error, t, sub, subscribe, context, stream));
}

void Subscribe::streamWorker(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
    boost::asio::io_context &initial_update_io,
    boost::asio::io_context &incr_update_io)
{
  vector<std::shared_ptr<boost::asio::steady_timer>> timers;

  for (int i = 0; i < request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    switch (sub.mode()) {
      case SAMPLE: {
        auto t = std::make_shared<boost::asio::steady_timer>(incr_update_io, nanoseconds{sub.sample_interval()});
        t->async_wait(boost::bind(sample_timer_expiry, boost::asio::placeholders::error, t, sub, this, context, stream));
        timers.push_back(t);
        break;
      }
      default:
        break;
    }
  }

  // Keep io_context running regardless if there are tasks to execute or not
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> initial_work_guard(initial_update_io.get_executor());

  initial_update_io.run();

  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> incr_work_guard(incr_update_io.get_executor());

  incr_update_io.run();

  BOOST_LOG_TRIVIAL(debug) << "Subscription stream worker exiting";
}

static void streamWorkerThread(Subscribe *sub, ServerContext* context, SubscribeRequest &request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
    std::tuple<boost::asio::io_context &, boost::asio::io_context &> io_context_tuple)
{
  boost::asio::io_context &initial_update_io = std::get<0>(io_context_tuple);
  boost::asio::io_context &incr_update_io = std::get<1>(io_context_tuple);
  sub->streamWorker(context, request, stream, initial_update_io, incr_update_io);
}

class SrModuleOnChangeParams {
public:
  SrModuleOnChangeParams(SubscribeRequest *request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest> *stream, Subscribe *subscribe,
    boost::asio::io_context &initial_update_io_context,
    boost::asio::io_context &incr_update_io_context) :
    request(request), stream(stream), subscribe(subscribe), initial_update_io_context(initial_update_io_context),
    incr_update_io_context(incr_update_io_context) {}

  SubscribeRequest *request;
  ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream;
  Subscribe *subscribe;
  boost::asio::io_context &initial_update_io_context;
  boost::asio::io_context &incr_update_io_context;

  bool is_incremental(void) const {
    // set incr_update=true and return the previous value.
    return std::exchange(incr_update, true);
  }
private:
  mutable bool incr_update = false;
};

sysrepo::ErrorCode srModuleOnChange(
    sysrepo::Session session, std::string_view module_name, std::string_view xpath, sysrepo::Event event,
    uint32_t request_id, const SrModuleOnChangeParams &params)
{
  Status status;
  auto response = make_unique<SubscribeResponse>();

  (void)module_name;
  (void)event;
  (void)request_id;

  string changes_path(xpath);

  status = params.subscribe->BuildSubscribeNotificationForChanges(response->mutable_update(),
                                      params.request->subscribe(), changes_path, session);
  if (!status.ok()) {
    BOOST_LOG_TRIVIAL(warning) << "unable to build update in response to notification for " << xpath;
    return sysrepo::ErrorCode::Ok;
  }

  if (params.is_incremental()) {
    params.subscribe->PostWrite(params.stream, std::move(response), params.incr_update_io_context);
  } else {
    params.subscribe->PostWrite(params.stream, std::move(response), params.initial_update_io_context);
  }

  return sysrepo::ErrorCode::Ok;
}

Status Subscribe::registerStreamOnChange(
    SubscribeRequest &request, Subscription sub,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
    boost::asio::io_context &initial_update_io_context,
    boost::asio::io_context &incr_update_io_context,
    shared_ptr<DataSubscribe> sr_sub,
    vector<SrModuleOnChangeParams> &params_vec)
{
  string fullpath = "";
  try {
    if (request.subscribe().prefix().elem_size() > 0 ||
        request.subscribe().prefix().target().compare("")) {
      fullpath = gnmi_to_xpath(request.subscribe().prefix());
    }
    fullpath += gnmi_to_xpath(sub.path());
  } catch (invalid_argument &exc) {
    BOOST_LOG_TRIVIAL(error) << exc.what();
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  }

  BOOST_LOG_TRIVIAL(debug) << "Subscribe (stream) " << fullpath;

  SrModuleOnChangeParams params(&request, stream, this, initial_update_io_context, incr_update_io_context);
  params_vec.push_back(params);
  try {
    auto params_ref = params_vec.back();
    sr_sub->data_change_subscribe(
      [params_ref]
      (sysrepo::Session session, uint32_t sub_id, std::string_view module_name, std::optional<std::string_view> xpath, sysrepo::Event event, uint32_t request_id)
      {
        (void)sub_id;
        return srModuleOnChange(session, module_name, xpath.value(), event, request_id, params_ref);
      },
      fullpath.c_str(),
      0, sysrepo::SubscribeOptions::Passive | sysrepo::SubscribeOptions::DoneOnly | sysrepo::SubscribeOptions::Enabled);
  } catch (const sysrepo::ErrorWithCode &exc) {
    BOOST_LOG_TRIVIAL(error) << exc.what();
    return Status(StatusCode::INTERNAL, exc.what());
  }

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with STREAM subscription mode by
 * periodically sending updates to the client.
 */
Status Subscribe::handleStream(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeResponse response;
  Status status;
  vector<SrModuleOnChangeParams> params_vec;

  if (request.subscribe().subscription_size() == 0) {
    return Status(StatusCode::INVALID_ARGUMENT,
                  "No subscription in message");
  }
  // Checks that sample_interval values are not higher than INT64_MAX
  // i.e. 9223372036854775807 nanoseconds
  for (int i = 0; i < request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    if (sub.sample_interval() > static_cast<uint64_t>(duration<long long, std::nano>::max().count()))
      return Status(StatusCode::INVALID_ARGUMENT,
                    string("sample_interval must be less than ")
                    + to_string(INT64_MAX) + " nanoseconds");

    if (sub.mode() == SubscriptionMode::SAMPLE && nanoseconds{sub.sample_interval()} < milliseconds(200)) {
      BOOST_LOG_TRIVIAL(warning) << "sample_interval " + to_string(sub.sample_interval()) +
                    " must be greater than " + to_string(nanoseconds{milliseconds(200)}.count()) +
                    " nanoseconds";
      return Status(StatusCode::INVALID_ARGUMENT,
                    string("sample_interval ") + to_string(sub.sample_interval()) +
                    " must be greater than " + to_string(nanoseconds{milliseconds(200)}.count()) +
                    " nanoseconds");
    }

  }

  // Get the initial data only for sample subscriptions
  bool sample=false;
  status = BuildSubscribeNotification(response.mutable_update(),
                                      request.subscribe(),
				      &sample);
  if (!status.ok())
    return status;

  boost::asio::io_context initial_update_io_context;
  boost::asio::io_context incr_update_io_context;

  SessionDsSwitcher ds_switch(sr_sess, sysrepo::Datastore::Operational);
  auto sr_sub = std::make_shared<DataSubscribe>(sr_sess);

  if (sample) {
    BOOST_LOG_TRIVIAL(debug) << "Sending initial update for sample subscriptions with size:" << response.update().update_size();
    // Sends a first Notification message that updates all sample subcriptions
    Write(stream, response);
  }
  for (int i=0; i<request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    switch (sub.mode()) {
      case SAMPLE:
        BOOST_LOG_TRIVIAL(debug) << "Subscribe (stream sample) " << gnmi_to_xpath(sub.path());
        break;
      case TARGET_DEFINED:
      case ON_CHANGE:
        status = registerStreamOnChange(request, sub, stream, initial_update_io_context, incr_update_io_context,
          sr_sub, params_vec);
        if (!status.ok()) {
          return status;
        }
        break;
      default:
        BOOST_LOG_TRIVIAL(warning) << "subscription mode " + to_string(sub.mode()) + " not implemented";
        return Status(StatusCode::UNIMPLEMENTED,
                      string("subscription mode " + to_string(sub.mode()) + " not implemented"));
    }
  }

  // Send to the worker thread
  boost::asio::post(initial_update_io_context, [&]
  {
    // Sends a SYNC message that indicates that initial synchronization
    // has completed, i.e. each Subscription has been updated once
    SubscribeResponse response;
    response.set_sync_response(true);
    BOOST_LOG_TRIVIAL(debug) << "Sending sync response";
    Write(stream, response);
    initial_update_io_context.stop();
  });

  // Start a worker thread for SAMPLE and ON_CHANGE notifications (the only other type, TARGET_DEFINED, isn't
  // supported).
  auto thread = std::thread(streamWorkerThread, this, context, std::ref(request), stream,
    std::make_tuple(std::ref(initial_update_io_context), std::ref(incr_update_io_context)));

  // Read from client - note that isn't expected to succeed, but allows us to
  // wait (without a busy loop) until the client cancels the streaming subscription and
  // then we can terminate the worker thread immediately
  SubscribeRequest request2;
  auto success = stream->Read(&request2);

  incr_update_io_context.stop();
  thread.join();

  if (success) {
    BOOST_LOG_TRIVIAL(warning) << "out-of-order operation was requested on a STREAM subscription";
    return Status(StatusCode::INVALID_ARGUMENT,
                  string("out-of-order operation was requested on a STREAM subscription"));
  }

  return Status::OK;
}

void Subscribe::Write(
  ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
  SubscribeResponse response)
{
  const std::lock_guard<std::recursive_mutex> lock(stream_mutex);
  stream->Write(response);
}

void Subscribe::PostWrite(
  ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream,
  std::unique_ptr<SubscribeResponse> response, boost::asio::io_context &io_context)
{
  // Send to the worker thread
  boost::asio::post(io_context, [this, stream, response = std::move(response)]
  {
    Write(stream, *response);
  });
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating
 * all the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
Status Subscribe::handleOnce(SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  Status status;

  // Sends a Notification message that updates all Subcriptions once
  SubscribeResponse response;
  status = BuildSubscribeNotification(response.mutable_update(),
                                      request.subscribe());
  if (!status.ok())
    return status;

  Write(stream, response);
  response.Clear();

  // Sends a message that indicates that initial synchronization
  // has completed, i.e. each Subscription has been updated once
  response.set_sync_response(true);
  Write(stream, response);
  response.Clear();

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating
 * all the Subscriptions each time a Poll request is received.
 */
Status Subscribe::handlePoll(SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest subscription = request;
  Status status;

  while (stream->Read(&request)) {
    switch (request.request_case()) {
      case request.kPoll:
        {
          // Sends a Notification message that updates all Subcriptions once
          SubscribeResponse response;
          status = BuildSubscribeNotification(response.mutable_update(),
                                              subscription.subscribe());
          if (!status.ok())
            return status;
          Write(stream, response);
          response.Clear();

          // Reference 3.5.2.3:
          // "For POLL subscriptions, after each set of updates for individual poll request, a SubscribeResponse message with the sync_response field set to true MUST be generated."
          response.set_sync_response(true);
          Write(stream, response);
          break;
        }
      case request.kAliases:
        return Status(StatusCode::UNIMPLEMENTED, "Aliases not implemented yet");
      case request.kSubscribe:
        return Status(StatusCode::INVALID_ARGUMENT,
                      "A SubscriptionList has already been received for this RPC");
      default:
        return Status(StatusCode::INVALID_ARGUMENT,
                      "Unknown content for SubscribeRequest message");
    }
  }

  return Status::OK;
}

/**
 * Handles the first SubscribeRequest message.
 * If it does not have the "subscribe" field set, the RPC MUST be cancelled.
 * Ref: 3.5.1.1
 */
Status Subscribe::run(ServerContext* context,
                 ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest request;

  stream->Read(&request);

  if (request.extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  if (!request.has_subscribe())
    return Status(StatusCode::INVALID_ARGUMENT,
                  "SubscribeRequest needs non-empty SubscriptionList");

  switch (request.subscribe().mode()) {
    case SubscriptionList_Mode_STREAM:
      return handleStream(context, request, stream);
    case SubscriptionList_Mode_ONCE:
      return handleOnce(request, stream);
    case SubscriptionList_Mode_POLL:
      return handlePoll(request, stream);
    default:
      BOOST_LOG_TRIVIAL(error) << "Unknown subscription mode";
      return Status(StatusCode::UNIMPLEMENTED, "Unknown subscription mode");
  }

  return Status::OK;
}

}
