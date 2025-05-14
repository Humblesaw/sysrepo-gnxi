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

#ifndef _ENCODE_H
#define _ENCODE_H

#include <proto/gnmi.grpc.pb.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>

using std::shared_ptr;
using std::string;
using std::vector;

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
class SessionDsSwitcher {
  public:
    SessionDsSwitcher(sysrepo::Session sess, sysrepo::Datastore ds)
      : sr_sess(sess)
    {
      orig_ds = sr_sess.activeDatastore();
      sr_sess.switchDatastore(ds);
    }
    ~SessionDsSwitcher()
    {
      sr_sess.switchDatastore(orig_ds);
    }
  private:
    sysrepo::Session sr_sess;
    sysrepo::Datastore orig_ds;
};

/*
 * Purpose for the encode/decode
 */
enum class EncodePurpose {
  Set,
  Rpc,
};

/*
 * Factory to instantiate encodings
 * Encoding can be {JSON, Bytes, Proto, ASCII, JSON_IETF}
 */
class Encode {
  public:
    Encode(sysrepo::Session sess)
      : sr_sess(sess)
    {
    }

    void set_log_id(uint64_t id) {
      log_id = id;
      sr_session_set_nc_id(sysrepo::getRawSession(sr_sess), id);
    }

    /* Supported Encodings */
    enum Supported {
      JSON_IETF = 0,
    };

    std::tuple<grpc::Status, std::optional<libyang::DataNode>> decode(string xpath, const gnmi::TypedValue &reqval, EncodePurpose purpose);
    std::tuple<grpc::Status, std::optional<libyang::DataNode>> update(string xpath, const gnmi::TypedValue &reqval, string op);
    grpc::Status encode(gnmi::Encoding encoding, libyang::DataNode node, gnmi::TypedValue *val);

    /* JSON encoding */
    std::optional<libyang::DataNode> json_decode(string xpath, string data, EncodePurpose purpose);
    string json_encode(libyang::DataNode node);

  private:
    sysrepo::Session sr_sess;
    uint64_t log_id = 0;
};

#endif //_ENCODE_H
