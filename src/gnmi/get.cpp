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

#include <grpc/grpc.h>

#include "get.h"
#include "encode/encode.h"
#include <utils/utils.h>
#include <utils/log.h>
#include <sysrepo-cpp/utils/exception.hpp>

using namespace std;
using google::protobuf::RepeatedPtrField;

namespace impl {

Status
Get::BuildGetUpdate(RepeatedPtrField<Update>* updateList,
                    string fullpath, gnmi::Encoding encoding)
{
  try {
    /* Get multiple subtree for YANG lists or one for other YANG types */
    auto sr_trees = sr_sess.getData(fullpath.c_str());
    /* The path not (yet) existing isn't an error, so just return an empty set */
    if (!sr_trees.has_value())
      return Status::OK;

    for (auto n : sr_trees->findXPath(fullpath.c_str())) {
      auto update = updateList->Add();
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
                              << exc.what();
    updateList->Clear();
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  }

  return Status::OK;
}

/*
 * Build Get Notifications - Build a Notification message.
 * Contrary to Subscribe Notifications, A new notification message must be
 * created for every path of the GetRequest.
 * There can still be multiple paths in GetResponse if requested path
 * is a directory path.
 *
 * IMPORTANT : we have choosen to have a stateless implementation of
 * gNMI so deleted path in Notification message will always be empty.
 */
Status
Get::BuildGetNotification(Notification *notification, const Path &prefix,
                          const Path &path, gnmi::Encoding encoding,
                          gnmi::GetRequest_DataType dataType)
{
  /* Data elements that have changed values */
  RepeatedPtrField<Update>* updateList = notification->mutable_update();
  string fullpath = "";
  auto ds = sysrepo::Datastore::Operational;

  /* Get time since epoch in milliseconds */
  notification->set_timestamp(get_time_nanosec());

  if (prefix.elem_size() > 0 || prefix.target().compare("")) {
    string str;
    try {
      str = gnmi_to_xpath(prefix);
    } catch (invalid_argument &exc) {
      return Status(StatusCode::INVALID_ARGUMENT, exc.what());
    }
    BOOST_LOG_TRIVIAL(debug) << "prefix is " << str;
    // gNMI spec §2.2.2.1:
    // When set in the prefix in a request, GetRequest, SetRequest or
    // SubscribeRequest, the field MUST be reflected in the prefix of the
    // corresponding GetResponse, SetResponse or SubscribeResponse by a
    // server.
    notification->mutable_prefix()->set_target(prefix.target());
    if (prefix.elem_size() > 0) {
      fullpath += str;
    }
  }

  try {
    gnmi_check_origin(prefix, path);
    fullpath += gnmi_to_xpath(path);
  } catch (invalid_argument &exc) {
    return Status(StatusCode::INVALID_ARGUMENT, exc.what());
  }
  BOOST_LOG_TRIVIAL(debug) << "GetRequest Path " << fullpath;

  if (dataType == gnmi::GetRequest_DataType_CONFIG)
    ds = sysrepo::Datastore::Running;

  SessionDsSwitcher ds_switch(sr_sess, ds);

  return BuildGetUpdate(updateList, fullpath, encoding);
}

/* Verify request fields are correct */
static inline Status verifyGetRequest(const GetRequest *request)
{
  switch (request->encoding()) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      break;

    default:
      BOOST_LOG_TRIVIAL(warning) << "Unsupported Encoding "
                                 << Encoding_Name(request->encoding());
      return Status(StatusCode::UNIMPLEMENTED,
                    Encoding_Name(request->encoding()));
  }

  if (!GetRequest_DataType_IsValid(request->type())) {
    BOOST_LOG_TRIVIAL(warning) << "Invalid Data Type in Get Request "
                               << GetRequest_DataType_Name(request->type());
    return Status(StatusCode::UNIMPLEMENTED,
                  GetRequest_DataType_Name(request->type()));
  }

  if (request->use_models_size() > 0) {
    BOOST_LOG_TRIVIAL(warning) << "use_models unsupported, ALL are used";
    return Status(StatusCode::UNIMPLEMENTED, "use_model feature unsupported");
  }

  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(warning) << "extension unsupported";
    return Status(StatusCode::UNIMPLEMENTED, "extension feature unsupported");
  }

  return Status::OK;
}

/* Implement gNMI Get RPC */
Status Get::run(const GetRequest* req, GetResponse* response)
{
  RepeatedPtrField<Notification> *notificationList;
  Notification *notification;
  Status status;

  status = verifyGetRequest(req);
  if (!status.ok())
    return status;

  BOOST_LOG_TRIVIAL(debug) << "GetRequest DataType "
                           << GetRequest::DataType_Name(req->type()) << ","
                           << "GetRequest Encoding "
                           << Encoding_Name(req->encoding());

  /* Run through all paths */
  notificationList = response->mutable_notification();
  for (auto path : req->path()) {
    notification = notificationList->Add();

    status = BuildGetNotification(notification, req->prefix(), path,
                                  req->encoding(), req->type());
    if (!status.ok()) {
      BOOST_LOG_TRIVIAL(error) << "Fail building get notification: "
                               << status.error_message();
      return status;
    }
  }

  return Status::OK;
}

}
