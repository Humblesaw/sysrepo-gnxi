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

#include "gnmi.h"
#include <utils/log.h>

using namespace gnmi;
using namespace std;
using google::protobuf::FileOptions;

Status GNMIService::Capabilities(ServerContext *context,
                                 const CapabilityRequest* request,
                                 CapabilityResponse* response)
{
  (void)context;
  string gnmi_version;
  FileOptions fopts;

  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  try {
    auto sess = sr_con.sessionStart();
    auto node = sess.getModuleInfo();
    for (auto mod_node : node.child()->siblings()) {
        auto model = response->add_supported_models();
        for (auto mod_value_node = mod_node.child(); mod_value_node.has_value();
             mod_value_node = mod_value_node.value().nextSibling()) {
          if (!mod_value_node->schema().name().compare("name")) {
            auto name = std::string(mod_value_node->asTerm().valueStr());
            model->set_name(name);
          }
          if (!mod_value_node->schema().name().compare("revision")) {
            auto version = std::string(mod_value_node->asTerm().valueStr());
            model->set_version(version);
          }
        }
    }

    gnmi_version = response->GetDescriptor()->file()->options()
                            .GetExtension(gnmi::gnmi_service);
    response->set_gnmi_version(gnmi_version);

    //Encoding used in TypedValue for responses
    //response->add_supported_encodings(gnmi::Encoding::JSON);
    //response->add_supported_encodings(gnmi::Encoding::BYTES);
    //response->add_supported_encodings(gnmi::Encoding::PROTO);
    //response->add_supported_encodings(gnmi::Encoding::ASCII);
    response->add_supported_encodings(gnmi::Encoding::JSON_IETF);

  } catch (const exception &exc) {
    BOOST_LOG_TRIVIAL(error) << exc.what();
    return Status(StatusCode::INTERNAL, "Fail getting schemas");
  }

  return Status::OK;
}
