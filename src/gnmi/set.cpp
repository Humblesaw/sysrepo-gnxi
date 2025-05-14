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

#include "set.h"
#include "confirm.h"
#include "encode/encode.h"
#include <utils/utils.h>
#include <utils/log.h>
#include <utils/sysrepo.h>
#include <sysrepo.h>
#include <sysrepo-cpp/utils/exception.hpp>

using namespace sysrepo;
using namespace std;
using namespace libyang;

namespace impl {

std::tuple<grpc::Status, std::optional<libyang::DataNode>> Set::handleUpdate(Update in, UpdateResult *out, string prefix_str, const Path &prefix, string op)
{
  //Parse request
  if (!in.has_path() || !in.has_val())
    return std::make_tuple(grpc::Status(StatusCode::INVALID_ARGUMENT, "Update no path or value"), std::nullopt);

  string fullpath;
  try {
    gnmi_check_origin(prefix, in.path());
    if (prefix.elem_size() > 0) {
      fullpath += prefix_str;
    }
    fullpath += gnmi_to_xpath(in.path());
  } catch (invalid_argument &exc) {
    return std::make_tuple(grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what()), std::nullopt);
  }
  BOOST_LOG_TRIVIAL(debug) << "Update (" << op << ") " << fullpath;

  auto result = encodef->update(fullpath, in.val(), op);

  //Fill in Reponse
  out->set_allocated_path(in.release_path());

  return result;
}

grpc::Status Set::run(const SetRequest* request, SetResponse* response)
{
  std::string prefix = "";
  std::vector<UpdateResult> results;
  UpdateTransaction xact;
  auto ietf_nc_mod = sr_sess.getContext().getModuleImplemented("ietf-netconf").value();

  // For observability, set transaction_id as log_id for subscribers and gnmi logging.
  encodef->set_log_id(request->transaction_id());

  // Check if we're waiting for a Confirm RPC
  if (conf_state->get_wait_confirm()) {
    return grpc::Status(StatusCode::UNAVAILABLE, "Previous Set has to be confirmed");
  }

  // Check if Set requires Confirm
  if (request->has_confirm()) {
    const ConfirmParmsRequest &conf_parms = request->confirm();
    BOOST_LOG_TRIVIAL(debug)
        << "Confirm msg has timeout=" << conf_parms.timeout_secs();
    BOOST_LOG_TRIVIAL(debug)
        << "Confirm msg has ignore-system-state: " << conf_parms.ignore_system_state();

    if (not conf_parms.ignore_system_state()) {
      // We have to check system state
    }

    // This (re)starts the timer, so as long as work here lasts less than
    // timeout...
    std::string err_msg = "";
    if (not conf_state->set_wait_confirm(conf_parms.timeout_secs(), err_msg)) {
      // Because of check above, should happen only in race condition
      return grpc::Status(StatusCode::UNAVAILABLE, err_msg);
    }

    // Add ConfirmParmsResponse in SetResponse
    ConfirmParmsResponse confirm;
    confirm.set_min_wait_secs(conf_state->get_min_wait_conf_secs());
    confirm.set_timeout_secs(conf_state->get_timeout_secs());
    response->mutable_confirm()->CopyFrom(confirm);

  }

  if (request->extension_size() > 0) {
    conf_state->clr_wait_confirm();
    return grpc::Status(StatusCode::UNIMPLEMENTED, "not supported");
  }

  response->set_timestamp(get_time_nanosec());

  /* Prefix for gNMI path */
  if (request->has_prefix()) {
    try {
      prefix = gnmi_to_xpath(request->prefix());
    } catch (invalid_argument &exc) {
      BOOST_LOG_TRIVIAL(error) << exc.what()
			       << ". Transaction-id:"
			       << request->transaction_id();
      conf_state->clr_wait_confirm();
      return grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what());
    }
    BOOST_LOG_TRIVIAL(debug) << "prefix is " << prefix;
    response->mutable_prefix()->CopyFrom(request->prefix());
  }

  /* gNMI paths to delete */
  if (request->delete__size() > 0) {

    // sort the paths to delete in reverse order to not delete children after parents
    std::set<std::string, std::greater<std::string>> del_paths;
    for (auto delpath : request->delete_()) {
      // Parse request and config sysrepo
      string fullpath;
      try {
        gnmi_check_origin(request->prefix(), delpath);
        fullpath = prefix + gnmi_to_xpath(delpath);
        del_paths.insert(fullpath);
      } catch (invalid_argument &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
			         << ". Transaction-id:"
			         << request->transaction_id();
	    conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what());
      }

      // Fill in Reponse
      UpdateResult res;
      *(res.mutable_path()) = delpath;
      res.set_op(gnmi::UpdateResult::DELETE);
      results.push_back(res);
    }

    for (auto &fullpath : del_paths) {
      BOOST_LOG_TRIVIAL(debug) << "Delete " << fullpath;
      try {
        // We cannot use deleteItem here as sysrepo doesn't like it being
        // mixed with edit_batch, so retrieve just the nodes referenced by
        // the xpath and no deeper to avoid it being any more expensive
        // than it has to be
        auto del_root = sr_sess.getData(fullpath, 1);
        if (!del_root.has_value())
          throw invalid_argument("xpath \"" + fullpath + "\" not found");

        if (fullpath.compare("/*") == 0) {
          // Walk all siblings and add delete node to them
          for (auto n : del_root->siblings()) {
            n.newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");

            BOOST_LOG_TRIVIAL(debug) << " 1. Delete path: " << n.path();
          }
        } else {
          // Find the node(s) actually referenced by the path to mark them as
          // requiring delete since they could well be deeper than the root
          // node
          auto set = del_root->findXPath(fullpath.c_str());
          if (set.empty())
            throw invalid_argument("xpath \"" + fullpath + "\" not found");

          auto sr_mod = sr_sess.getContext().getModuleImplemented("sysrepo").value();
          for (auto n : set) {
              // Ensure we don't create any parent nodes - they might be deleted
              // in this transaction.
              auto p = n.parent();
              while (p.has_value()) {
                p->newMeta(sr_mod, "sysrepo:operation", "ether");
                p = p->parent();
              }

            n.newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");
            BOOST_LOG_TRIVIAL(debug) << " 2. Delete path: " << n.path();
          }
        }
        xact.push(*del_root);
      } catch (const invalid_argument &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what();
        // gNMI spec §3.4.6: In the case that a path specifies an element within the data tree that does not exist, these deletes MUST be silently accepted.
      } catch (const exception &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what());
      }
   }
  }

  /* gNMI paths with value to replace */
  if (request->replace_size() > 0) {
    for (auto &upd : request->replace()) {
      UpdateResult res;
      try {
        auto [status, node] = handleUpdate(upd, &res, prefix, request->prefix(), "replace");
        if (!status.ok()) {
          BOOST_LOG_TRIVIAL(error) << "Fail building set notification: "
                                   << status.error_message()
				   << ". Transaction-id: "
				   << request->transaction_id();
          conf_state->clr_wait_confirm();
          return status;
        }

        res.set_op(gnmi::UpdateResult::REPLACE);
        results.push_back(res);
        if (node.has_value()) {
          xact.push(*node);
        }
      } catch (const invalid_argument &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what());
      } catch (sysrepo::Error &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INTERNAL, exc.what());
      } catch (const exception &exc) { //Any other exception
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INTERNAL, exc.what());
      }
    }
  }

  /* gNMI paths with value to update */
  if (request->update_size() > 0) {
    for (auto &upd : request->update()) {
      UpdateResult res;
      try {
        auto [status, node] = handleUpdate(upd, &res, prefix, request->prefix(), "merge");
        if (!status.ok()) {
          BOOST_LOG_TRIVIAL(error) << "Fail building set notification: "
                                   << status.error_message()
				   << ". Transaction-id: "
				   << request->transaction_id();
          conf_state->clr_wait_confirm();
          return status;
        }
        res.set_op(gnmi::UpdateResult::UPDATE);
        results.push_back(res);
        if (node.has_value()) {
            xact.push(*node);
        }
      } catch (const invalid_argument &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INVALID_ARGUMENT, exc.what());
      } catch (const sysrepo::Error &exc) {
        BOOST_LOG_TRIVIAL(error) << exc.what()
				 << ". Transaction-id:"
				 << request->transaction_id();
        conf_state->clr_wait_confirm();
        return grpc::Status(StatusCode::INTERNAL, exc.what());
      }
    }
  }

  try {
    if (xact.first_node.has_value()) {
      sr_sess.editBatch(xact.first_node.value(), sysrepo::DefaultOperation::Merge);
    }

    sr_sess.applyChanges();

    if (!request->has_confirm()) {
      sr_sess_startup.copyConfig(sysrepo::Datastore::Running);
    }
    conf_state->write_set_transaction_id(request->transaction_id());
  } catch (const sysrepo::Error &exc) {
    conf_state->clr_wait_confirm();
    string err_str;
    auto errors = sr_sess.getErrors();
    if (errors.size()) {
      err_str = errors[0].errorMessage;
    } else {
      err_str = exc.what();
    }
    sr_sess.discardChanges();
    BOOST_LOG_TRIVIAL(error) << "commit error: "
			     << err_str
			     << ". Transaction-id:"
			     << request->transaction_id();
    return grpc::Status(StatusCode::ABORTED, err_str);
  } catch (const exception &exc) {
    BOOST_LOG_TRIVIAL(error) << exc.what()
			     << ". Transaction-id:"
			     << request->transaction_id();
    sr_sess.discardChanges();
    return grpc::Status(StatusCode::INTERNAL, exc.what());
  }

  for (auto r : results)
    *(response->add_response()) = r;

  conf_state->reset_timers();
  return grpc::Status::OK;
}

} // namespace impl
