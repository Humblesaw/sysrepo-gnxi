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

#include <tuple>
#include "encode.h"
#include "utils/log.h"
#include "utils/sysrepo.h"

using namespace gnmi;
using namespace std;
using namespace grpc;
using Status = grpc::Status;

std::tuple<grpc::Status, std::optional<libyang::DataNode>> Encode::decode(
  string xpath, const gnmi::TypedValue &reqval, EncodePurpose purpose)
{
  switch (reqval.value_case()) {
    case gnmi::TypedValue::ValueCase::kStringVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf string type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kIntVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf int type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kUintVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf uint type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kBoolVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf bool type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kBytesVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf bytes type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kFloatVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf float type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kDecimalVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf Decimal64 type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kLeaflistVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported protobuf leaflist type"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kAnyVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported PROTOBUF Encoding"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kJsonVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported JSON Encoding"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kJsonIetfVal:
      try {
        return std::make_tuple(Status::OK, json_decode(xpath, reqval.json_ietf_val(), purpose));
      } catch (runtime_error &err) {
        // wrong input field must reply an error to gnmi client
        BOOST_LOG_TRIVIAL(error) << "Run-time error:" << err.what();
        return std::make_tuple(Status(StatusCode::INVALID_ARGUMENT, err.what()), std::nullopt);
      } catch (invalid_argument &err) {
        BOOST_LOG_TRIVIAL(error) << "Invalid argument:" << err.what();
        return std::make_tuple(Status(StatusCode::INVALID_ARGUMENT, err.what()), std::nullopt);
      }
      break;
    case gnmi::TypedValue::ValueCase::kAsciiVal:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported ASCII Encoding"), std::nullopt);
    case gnmi::TypedValue::ValueCase::kProtoBytes:
      return std::make_tuple(Status(StatusCode::UNIMPLEMENTED, "Unsupported PROTOBUF BYTE Encoding"), std::nullopt);
    case gnmi::TypedValue::ValueCase::VALUE_NOT_SET:
      return std::make_tuple(Status(StatusCode::INVALID_ARGUMENT, "Value not set"), std::nullopt);
    default:
      return std::make_tuple(Status(StatusCode::INVALID_ARGUMENT, "Unknown value type"), std::nullopt);
  }
}

std::tuple<Status, std::optional<libyang::DataNode>> Encode::update(string xpath, const TypedValue &reqval, string op)
{
  UpdateTransaction xact;

  if (xpath.compare("/*") != 0 && op.compare("replace") == 0) {
    // Check if the xpath we are replacing is a leaf-list or a list
    auto node_type = sr_sess.getContext().findPath(xpath).nodeType();
    if (node_type == libyang::NodeType::Leaflist || node_type == libyang::NodeType::List) {
      // Replacing list or leaflist means we should delete all previous entries
      auto created_nodes = sr_sess.getContext().newPath2(xpath, std::nullopt, libyang::CreationOptions::Opaque);
      auto del_parent = created_nodes.createdParent.value();
      auto del_node = created_nodes.createdNode.value();
      if (del_node.isOpaque()) {
        del_node.newAttrOpaqueJSON("sysrepo", "operation", "purge");
      } else {
        auto sr_mod = sr_sess.getContext().getModuleImplemented("sysrepo").value();
        // libyang treats NULL as a valid value for some data types
        del_node.newMeta(sr_mod, "sysrepo:operation", "purge");
      }
      xact.push(del_parent);
    }
  }

  auto [status, node] = decode(xpath, reqval, EncodePurpose::Set);
  if (!status.ok())
    return std::make_tuple(status, std::nullopt);

  auto root_node = node;
  auto edit_node = node;

  auto ietf_nc_mod = sr_sess.getContext().getModuleImplemented("ietf-netconf").value();
  if (xpath.compare("/*") == 0) {
    if (op.compare("replace") == 0) {
      // The gNMI semantics are that a replace at the top-level should cause all data node not provided to be removed.
      // However, sysrepo semantics are that only the provided nodes are replaced. Therefore, request that everything
      // not being replaced is deleted.

      auto del_root = sr_sess.getData(xpath.c_str(), 1);
      // Walk all siblings not in update and add delete node to them
      for (auto n = std::optional<libyang::DataNode>(del_root); n.has_value(); n = n->nextSibling()) {
        if (getRawNode(*n)->flags & LYD_DEFAULT) {
          // Default nodes need not be deleted and can be skipped
          continue;
        }

        bool is_replace_node = false;
        // Is this node a replace node?
        for (auto repl_n = edit_node; repl_n.has_value(); repl_n = repl_n->nextSibling()) {
          if (n->schema().path() == repl_n->schema().path()) {
            is_replace_node = true;
            break;
          }
        }

        // If this is a replace node, then optimise further sysrepo processing by not adding it to the batch
        if (!is_replace_node) {
          n->newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");
          xact.push_one(*n);
        }
      }
    }

    // Add operation attribute to each node - there can be multiple if the JSON contains multiple top-level nodes.
    for (auto n = edit_node; n.has_value(); n = n->nextSibling()) {
      n->newMeta(ietf_nc_mod, "ietf-netconf:operation", op);
    }
    if (edit_node.has_value()) {
        xact.push(edit_node.value());
    }
    root_node = xact.first_node;

  } else {
    // Find the edit point for the data fragment
    auto set = root_node->findXPath(xpath.c_str());
    // We should have found a path, and wildcards don't make sense
    if (set.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Empty result searching for "
				 << xpath.c_str();
        throw invalid_argument("invalid set returned for xpath \"" + xpath + "\"");
    }

    for (auto edit_node : set) {
        edit_node.newMeta(ietf_nc_mod, "ietf-netconf:operation", op);
        BOOST_LOG_TRIVIAL(debug) << op.c_str() << " path: " << edit_node.path();
    }
    xact.push(root_node.value());
    root_node = xact.first_node;
  }

  return std::make_tuple(Status::OK, root_node);
}

grpc::Status Encode::encode(Encoding encoding, libyang::DataNode node, TypedValue *val)
{
  switch (encoding) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      val->set_json_ietf_val(json_encode(node));
      break;
    default:
      BOOST_LOG_TRIVIAL(warning) << "Unsupported Encoding "
                                << Encoding_Name(encoding);
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(encoding));
  }

  return Status::OK;
}
