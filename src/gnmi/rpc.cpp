/*
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
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Connection.hpp>

#include "rpc.h"
#include <utils/log.h>
#include <utils/utils.h>

namespace impl
{

// Implements gNMI Rpc RPC
grpc::Status Rpc::run(const gnmi::RpcRequest *request, gnmi::RpcResponse *response)
{
    try
    {
        auto xpath = gnmi_to_xpath(request->path());

        // Convert RPC call timeout from seconds. Use 2000ms as default (per SR_RPC_CB_TIMEOUT)
        // Max timeout of 10000ms (ensure less than SR_MAIN_LOCK_TIMEOUT)
        uint32_t timeout = request->timeout() ? request->timeout() * 1000 : 2000;
        if (timeout > 10000)
        {
            timeout = 10000;
        }

        SLOG_DEBUG("Rpc RPC (", xpath, ") timeout ", timeout / 1000, "s");

        auto [status, input_node] = encodef->decode(xpath, request->val(), EncodePurpose::Rpc);

        if (!status.ok())
        {
            SLOG_WARN("Rpc input value error: ", status.error_message());
            return status;
        }

        auto output_node = sr_sess.sendRPC(input_node.value(), std::chrono::milliseconds(timeout));

        response->set_timestamp(get_time_nanosec());
        if (!output_node.has_value())
        {
            return grpc::Status::OK;
        }

        status = encodef->encode(request->encoding(), *output_node, response->mutable_val());
        if (!status.ok())
            SLOG_WARN("Rpc output value error: ", status.error_message());

        return status;
    }
    catch (std::invalid_argument &exc)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    catch (std::exception &ex)
    {
        std::string err_str;
        auto errors = sr_sess.getErrors();
        if (!errors.empty())
        {
            err_str = errors[0].errorMessage;
        }
        else
        {
            err_str = ex.what();
        }
        SLOG_WARN("RPC error: ", err_str);
        return grpc::Status(grpc::StatusCode::ABORTED, err_str);
    }
}

} // namespace impl
