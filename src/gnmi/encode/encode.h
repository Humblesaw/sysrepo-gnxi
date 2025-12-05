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

#pragma once

#include <libyang-cpp/DataNode.hpp>
#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>

/*
 * Encode directory aims at providing a CREATE-UPDATE-READ wrapper on top of
 * sysrepo for JSON encoding (other encodings can be added).
 * It provides YANG validation before storing elements and after fetching them
 * in sysrepo.
 *
 * update()  CREATE & UPDATE
 * read()    READ
 *
 * DELETE is not supported as it is not dependent of encodings.
 * Use sr_deleteItem to suppress subtree from a xpath directly.
 */

/* helper class to reset session datastore on going out of scope */
class SessionDsSwitcher
{
  public:
    SessionDsSwitcher(sysrepo::Session sess, sysrepo::Datastore ds) : sr_sess(sess)
    {
        orig_ds = sr_sess.activeDatastore();
        sr_sess.switchDatastore(ds);
    }
    ~SessionDsSwitcher() { sr_sess.switchDatastore(orig_ds); }

  private:
    sysrepo::Session sr_sess;
    sysrepo::Datastore orig_ds;
};

/*
 * Purpose for the encode/decode
 */
enum class EncodePurpose
{
    Set,
    Rpc,
};

/*
 * Factory to instantiate encodings
 * Encoding can be {JSON, Bytes, Proto, ASCII, JSON_IETF}
 */
class Encode
{
  public:
    Encode(sysrepo::Session sess) : sr_sess(sess) {}

    void set_log_id(uint64_t id)
    {
        // TODO doesnt work for now
        // const char *originator = "sysrepo_gnxi";
        // struct sr_session_ctx_s *session = getRawSession(sr_sess);

        /* store id */
        log_id = id;

        // if (!session)
        // {
        //     return;
        // }

        // if (!session->orig_name)
        // {
        //     sr_session_set_orig_name(session, originator);
        // }

        // /* Need to remove all previous data */
        // sr_session_del_orig_data(session);
        // sr_session_push_orig_data(session, sizeof id, &id);
    }

    /* Supported Encodings */
    enum Supported
    {
        JSON_IETF = 0,
    };

    std::tuple<grpc::Status, std::optional<libyang::DataNode>>
    decode(std::string xpath, const gnmi::TypedValue &reqval, EncodePurpose purpose);
    grpc::Status encode(gnmi::Encoding encoding, libyang::DataNode node, gnmi::TypedValue *val);

    /* JSON encoding */
    std::optional<libyang::DataNode> json_decode(std::string xpath, std::string data,
                                                 EncodePurpose purpose);
    std::string json_encode(libyang::DataNode node);

  private:
    sysrepo::Session sr_sess;
    uint64_t log_id = 0;
};
