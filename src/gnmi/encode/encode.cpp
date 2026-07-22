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

#include "encode.h"
#include "utils/log.h"
#include <proto/gnmi.grpc.pb.h>
#include <tuple>

std::tuple<grpc::Status, std::optional<libyang::DataNode>>
Encode::decode(std::string xpath, const gnmi::TypedValue &reqval, EncodePurpose purpose)
{
    switch (reqval.value_case())
    {
    case gnmi::TypedValue::ValueCase::kStringVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf string type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kIntVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf int type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kUintVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf uint type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kBoolVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf bool type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kBytesVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf bytes type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kFloatVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf float type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kDoubleVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf double type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kDecimalVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf Decimal64 type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kLeaflistVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported protobuf leaflist type"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kAnyVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported PROTOBUF Encoding"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kJsonVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported JSON Encoding"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kJsonIetfVal:
        try
        {
            return std::make_tuple(grpc::Status::OK,
                                   json_decode(xpath, reqval.json_ietf_val(), purpose));
        }
        catch (std::runtime_error &err)
        {
            // wrong input field must reply an error to gnmi client
            SLOG_ERROR("Run-time error:", err.what());
            return std::make_tuple(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err.what()),
                                   std::nullopt);
        }
        catch (std::invalid_argument &err)
        {
            SLOG_ERROR("Invalid argument:", err.what());
            return std::make_tuple(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err.what()),
                                   std::nullopt);
        }
        break;
    case gnmi::TypedValue::ValueCase::kAsciiVal:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported ASCII Encoding"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::kProtoBytes:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unsupported PROTOBUF BYTE Encoding"),
            std::nullopt);
    case gnmi::TypedValue::ValueCase::VALUE_NOT_SET:
        return std::make_tuple(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Value not set"),
                               std::nullopt);
    default:
        return std::make_tuple(
            grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Unknown value type"), std::nullopt);
    }
}

grpc::Status Encode::encode(gnmi::Encoding encoding, libyang::DataNode node, gnmi::TypedValue *val)
{
    switch (encoding)
    {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
        val->set_json_ietf_val(json_encode(node));
        break;
    default:
        SLOG_WARN("Unsupported Encoding ", Encoding_Name(encoding));
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, Encoding_Name(encoding));
    }

    return grpc::Status::OK;
}
