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

#include <string>
#include <sysrepo-cpp/Session.hpp>

#include <utils/log.h>
#include <utils/sysrepo.h>
#include <libyang/tree_data.h>

#include "encode.h"

using namespace std;
using namespace libyang;


/*****************
 * CRUD - UPDATE *
 *****************/

std::string stripQuotes(const std::string& str) {
    if (str.front() == '\"' && str.back() == '\"') {
        return str.substr(1, str.length() - 2);
    }
    return str;
}

/*
 * Parse a message encoded in JSON IETF and set fields in sysrepo.
 * @param data Input data encoded in JSON
 */
std::optional<libyang::DataNode> Encode::json_decode(string xpath, string data, EncodePurpose purpose)
{
  // Request to fail if the data doesn't match the schema
  auto metadata = "XPath: " + xpath + ". InputData";
  log_to_file(data, metadata, log_id);

  if (xpath.compare("/*") == 0) {
    try {
      auto ctx = sr_sess.getContext();
      return ctx.parseData(data, DataFormat::JSON, ParseOptions::ParseOnly | ParseOptions::Strict, std::nullopt);
    } catch (const exception &exc) {
      BOOST_LOG_TRIVIAL(error) << "Failed to parse data:" << obfs_data(data)
			       << ". Exception: " << exc.what();
      // Don't leave the error lying around on the context otherwise sysrepo may pick it up on an unrelated operation
      auto ctx = sr_sess.getContext();
      const_cast<libyang::Context *>(&ctx)->cleanAllErrors();
      throw invalid_argument(exc.what());
    }
  }

  std::optional<libyang::DataNode> root_node;

  // Create a node tree according to the xpath. The data is passed in because libyang makes this mandatory for leaf
  // nodes - it will be ignored for other node types (we cannot easily know what the node type is ahead of time).
  try {
    data = stripQuotes(data);
    auto schema_node = sr_sess.getContext().findPath(xpath);
    auto node_type = schema_node.nodeType();
    auto opts = CreationOptions::Update;

    if (node_type == libyang::NodeType::Leaflist) {
      opts = opts | CreationOptions::IgnoreInvalidValue;
    }

    if (node_type == libyang::NodeType::Leaf) {
      // For empty leaf, libyang expects "" and not "[null]"
      auto base_type = schema_node.asLeaf().valueType().base();
      if (base_type == libyang::LeafBaseType::Empty && data == "[null]") {
          data = "";
      }
    } else {
      // Only non-Leaf nodes can be opaque
      opts = opts | CreationOptions::Opaque;
    }

    auto created_nodes = sr_sess.getContext().newPath2(xpath, data, opts);
    root_node = created_nodes.createdParent;
    if (created_nodes.createdNode->schema().nodeType() == NodeType::Leaf) {
        /* If it is a leaf node, we are done here */
        return root_node;
    }
  } catch (const exception &exc) {
    BOOST_LOG_TRIVIAL(error) << "Failed to create node:" << xpath.c_str()
                            << "Exception: " << exc.what();
    // Don't leave the error lying around on the context otherwise sysrepo may pick it up on an unrelated operation
    auto ctx = sr_sess.getContext();
    const_cast<libyang::Context *>(&ctx)->cleanAllErrors();
    throw;
  }

  // Now find the edit point for the data fragment
  auto set = root_node->findXPath(xpath);
  // We should have found a path, and wildcards don't make sense
  if (set.size() != 1)
      throw invalid_argument("invalid set returned for xpath \"" + xpath + "\"");

  auto edit_node = set.front();

  try {
    if (purpose == EncodePurpose::Rpc) {
      edit_node.parseOp(data.c_str(), DataFormat::JSON, OperationType::RpcYang);
    } else {
      // Parse input JSON, expecting a fragment
      edit_node.parseData(data.c_str(), DataFormat::JSON, ParseOptions::ParseOnly | ParseOptions::Strict | ParseOptions::BareTopLeaf);
    }
  } catch (const exception &exc) {
    // Don't leave the error lying around on the context otherwise sysrepo may pick it up on an unrelated operation
    auto ctx = sr_sess.getContext();
    const_cast<libyang::Context *>(&ctx)->cleanAllErrors();
    BOOST_LOG_TRIVIAL(error) << "Failed to parse data. xpath: " << xpath
                 << ", data:" << obfs_data(data)
                 << ". Exception: " << exc.what();
    throw;
  }

  return root_node;
}

/***************
 * CRUD - READ *
 ***************/

/* Encode a libyang data node into JSON form */
string Encode::json_encode(libyang::DataNode node)
{
  string data;

  if (node.schema().nodeType() == NodeType::Leaf)
    data = node.printStr(DataFormat::JSON, PrintFlags::BareTopLeaf).value();
  else {
    // In case the node has no children
    data = "{}";
    // The xpath will have found the containing node, but we want to dump its children according to gNMI rules
    if (node.child().has_value()) {
      for (auto it : node.child()->childrenDfs()) {
        data = it.printStr(DataFormat::JSON, PrintFlags::WithSiblings | PrintFlags::Shrink | PrintFlags::Fragment).value();
        break;
      }
    }
  }

  return data;
}
