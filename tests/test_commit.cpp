/**
 * @file commit.cpp
 * @author Ondrej Kusnirik <kusnirik@cesnet.cz>
 * @brief Commit confirmed extension tests
 *
 * @copyright
 * Copyright (c) 2026 CESNET, z.s.p.o.
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

#include "catch2/catch.hpp"

#include <grpcpp/grpcpp.h>
#include <optional>
#include <thread>
#include <utility>

#include <proto/gnmi_ext.pb.h>

#include "gnmi/commit.h"
#include "test_main.h"

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

static const std::string test_xpath = "/gnmi-server-test:test/things[name='A']/enabled";

/**
 * @brief Fixture: cleans confirm state and sysrepo before and after
 * every test case, and provides helpers that build and send
 * the common Set/Commit requests.
 *
 */
class CommitFixture
{
  public:
    /**
     * @brief Result of the Set RPC call and its response.
     *
     */
    struct SetResult
    {
        grpc::Status status;
        gnmi::SetResponse response;
    };

    CommitFixture()
    {
        clean_confirm_state();
        clean_sysrepo();
    }

    ~CommitFixture()
    {
        clean_confirm_state();
        clean_sysrepo();
    }

    // set a leaf value in both Running and Startup datastores
    void set_initial_config(const std::string &xpath, const std::string &value)
    {
        sr_sess->switchDatastore(sysrepo::Datastore::Running);
        sr_sess->setItem(xpath.c_str(), value.c_str());
        sr_sess->applyChanges();
    }

    // verify a leaf value in a given datastore
    void check_value(sysrepo::Datastore ds, const std::string &xpath, const std::string &expected)
    {
        sr_sess->switchDatastore(ds);
        auto val = sr_sess->getOneNode(xpath);
        CHECK_THAT(std::string(val.asTerm().valueStr()), Equals(expected));
    }

    // verify a successful SetResponse carrying no UpdateResults
    // (confirm/cancel/set_rollback_duration)
    void check_empty_success_response(const SetResult &r)
    {
        CHECK(r.response.timestamp() > 0);
        CHECK(r.response.extension_size() == 0);
        CHECK(!r.response.has_prefix());
        CHECK(r.response.response_size() == 0);
    }

    // verify an error SetResponse: no timestamp, no UpdateResults
    void check_error_response(const SetResult &r)
    {
        CHECK(r.response.timestamp() == 0);
        CHECK(r.response.response_size() == 0);
    }

    // send a commit action, optionally with a mutation
    SetResult
    send_commit(const std::string &commit_id,
                const std::optional<std::pair<std::string, std::string>> &mutation = std::nullopt)
    {
        return send_commit_ext(
            commit_id, [](gnmi_ext::Commit *c) { c->mutable_commit(); }, mutation);
    }

    // send a commit action with explicit rollback duration, optionally with a mutation
    SetResult send_commit_with_rollback(
        const std::string &commit_id, int64_t duration_secs,
        const std::optional<std::pair<std::string, std::string>> &mutation = std::nullopt)
    {
        return send_commit_ext(
            commit_id, [duration_secs](gnmi_ext::Commit *c)
            { c->mutable_commit()->mutable_rollback_duration()->set_seconds(duration_secs); },
            mutation);
    }

    // send a confirm action, optionally with a mutation
    SetResult
    send_confirm(const std::string &commit_id,
                 const std::optional<std::pair<std::string, std::string>> &mutation = std::nullopt)
    {
        return send_commit_ext(
            commit_id, [](gnmi_ext::Commit *c) { c->mutable_confirm(); }, mutation);
    }

    // send a cancel action, optionally with a mutation
    SetResult
    send_cancel(const std::string &commit_id,
                const std::optional<std::pair<std::string, std::string>> &mutation = std::nullopt)
    {
        return send_commit_ext(
            commit_id, [](gnmi_ext::Commit *c) { c->mutable_cancel(); }, mutation);
    }

    // send a set_rollback_duration action, optionally with a mutation
    SetResult send_set_rollback_duration(
        const std::string &commit_id, int64_t secs,
        const std::optional<std::pair<std::string, std::string>> &mutation = std::nullopt)
    {
        return send_commit_ext(
            commit_id, [secs](gnmi_ext::Commit *c)
            { c->mutable_set_rollback_duration()->mutable_rollback_duration()->set_seconds(secs); },
            mutation);
    }

    // send a commit extension with no action set
    SetResult send_no_action(const std::string &commit_id)
    {
        return send_commit_ext(commit_id, [](gnmi_ext::Commit *) {}, std::nullopt);
    }

    // send an empty (unsupported) extension
    SetResult send_unsupported_extension()
    {
        grpc::ClientContext ctx;
        gnmi::SetRequest request;
        gnmi::SetResponse response;
        request.add_extension();
        return {client->Set(&ctx, request, &response), std::move(response)};
    }

    // send a plain Set (no extension) with a mutation
    SetResult send_plain_set(const std::string &xpath, const std::string &value)
    {
        grpc::ClientContext ctx;
        gnmi::SetRequest request;
        gnmi::SetResponse response;
        auto update = request.add_update();
        xpath_to_path(xpath, update->mutable_path());
        update->mutable_val()->set_json_ietf_val(value);
        return {client->Set(&ctx, request, &response), std::move(response)};
    }

  private:
    // build and send a SetRequest with a Commit extension configured by config_commit
    // optionally add a single mutation (xpath, value)
    template <typename ConfigFn>
    SetResult send_commit_ext(const std::string &commit_id, ConfigFn config_commit,
                              const std::optional<std::pair<std::string, std::string>> &mutation)
    {
        grpc::ClientContext ctx;
        gnmi::SetRequest request;
        gnmi::SetResponse response;
        auto commit = request.add_extension()->mutable_commit();
        commit->set_id(commit_id);
        config_commit(commit);
        if (mutation)
        {
            auto update = request.add_update();
            xpath_to_path(mutation->first, update->mutable_path());
            update->mutable_val()->set_json_ietf_val(mutation->second);
        }
        return {client->Set(&ctx, request, &response), std::move(response)};
    }

    // clear any active confirmed-commit state
    static void clean_confirm_state() { impl::Commit::get_singleton().clear(); }

    // remove test data from sysrepo
    static void clean_sysrepo()
    {
        sr_sess->switchDatastore(sysrepo::Datastore::Running);
        try
        {
            sr_sess->deleteItem("/gnmi-server-test:test/things[name='A']");
        }
        catch (...)
        {
        }
        try
        {
            sr_sess->deleteItem("/gnmi-server-test:test/things[name='B']");
        }
        catch (...)
        {
        }
        try
        {
            sr_sess->applyChanges();
        }
        catch (...)
        {
        }
    }
};

// positive tests

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit then confirm", "[commit]")
{
    set_initial_config(test_xpath, "true");

    // running initialized
    check_value(sysrepo::Datastore::Running, test_xpath, "true");

    // commit with mutation: enabled -> false
    auto r1 =
        send_commit_with_rollback("commit-confirm-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());
    CHECK(r1.response.timestamp() > 0);
    CHECK(r1.response.extension_size() == 0);
    CHECK(!r1.response.has_prefix());
    REQUIRE(r1.response.response_size() == 1);
    CHECK(r1.response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    CHECK(path_to_xpath(r1.response.response().Get(0).path()) == test_xpath);
    CHECK(impl::Commit::get_singleton().get_wait_confirm());

    // running changed
    check_value(sysrepo::Datastore::Running, test_xpath, "false");

    // confirm
    auto r2 = send_confirm("commit-confirm-1");
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(!impl::Commit::get_singleton().get_wait_confirm());
    check_value(sysrepo::Datastore::Running, test_xpath, "false");
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit then cancel (rollback)", "[commit]")
{
    set_initial_config(test_xpath, "true");

    check_value(sysrepo::Datastore::Running, test_xpath, "true");

    auto r1 =
        send_commit_with_rollback("commit-cancel-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());
    CHECK(r1.response.timestamp() > 0);
    CHECK(r1.response.extension_size() == 0);
    CHECK(!r1.response.has_prefix());
    REQUIRE(r1.response.response_size() == 1);
    CHECK(r1.response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    CHECK(path_to_xpath(r1.response.response().Get(0).path()) == test_xpath);
    CHECK(impl::Commit::get_singleton().get_wait_confirm());

    check_value(sysrepo::Datastore::Running, test_xpath, "false");

    auto r2 = send_cancel("commit-cancel-1");
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(!impl::Commit::get_singleton().get_wait_confirm());

    // config rolled back to original
    check_value(sysrepo::Datastore::Running, test_xpath, "true");
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit then set_rollback_duration", "[commit]")
{
    set_initial_config(test_xpath, "true");

    check_value(sysrepo::Datastore::Running, test_xpath, "true");

    auto r1 = send_commit_with_rollback("commit-srd-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());
    CHECK(r1.response.timestamp() > 0);
    CHECK(r1.response.extension_size() == 0);
    CHECK(!r1.response.has_prefix());
    REQUIRE(r1.response.response_size() == 1);
    CHECK(r1.response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    CHECK(path_to_xpath(r1.response.response().Get(0).path()) == test_xpath);
    CHECK(impl::Commit::get_singleton().get_wait_confirm());

    CHECK(impl::Commit::get_singleton().get_rollback_secs() == 120);

    auto r2 = send_set_rollback_duration("commit-srd-1", 300);
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(impl::Commit::get_singleton().get_rollback_secs() == 300);

    CHECK(impl::Commit::get_singleton().get_wait_confirm());
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit then timeout rollback", "[commit]")
{
    set_initial_config(test_xpath, "true");

    // commit with 2s rollback duration
    auto r1 = send_commit_with_rollback("commit-timeout-1", 2, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());
    CHECK(r1.response.timestamp() > 0);
    CHECK(r1.response.extension_size() == 0);
    CHECK(!r1.response.has_prefix());
    REQUIRE(r1.response.response_size() == 1);
    CHECK(r1.response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    CHECK(path_to_xpath(r1.response.response().Get(0).path()) == test_xpath);
    CHECK(impl::Commit::get_singleton().get_wait_confirm());

    check_value(sysrepo::Datastore::Running, test_xpath, "false");

    // wait for timer to expire (2s + buffer)
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // config rolled back
    check_value(sysrepo::Datastore::Running, test_xpath, "true");
    CHECK(!impl::Commit::get_singleton().get_wait_confirm());
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit with default rollback duration",
                 "[commit]")
{
    set_initial_config(test_xpath, "true");

    // commit without rollback_duration -> should default to 600s.
    auto r1 = send_commit("commit-default-1", std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());
    CHECK(r1.response.timestamp() > 0);
    CHECK(r1.response.extension_size() == 0);
    CHECK(!r1.response.has_prefix());
    REQUIRE(r1.response.response_size() == 1);
    CHECK(r1.response.response().Get(0).op() == gnmi::UpdateResult_Operation_UPDATE);
    CHECK(path_to_xpath(r1.response.response().Get(0).path()) == test_xpath);
    CHECK(impl::Commit::get_singleton().get_wait_confirm());

    CHECK(impl::Commit::get_singleton().get_rollback_secs() == 600);
}

// negative tests

// https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-commit-confirmed.md#321-commit

TEST_CASE_METHOD(CommitFixture, "Commit extension: empty commit id", "[commit-neg]")
{
    auto r = send_commit("");
    CHECK(r.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r.status.error_message(), Contains("commit id required"));
    check_error_response(r);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit while waiting", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("commit-wait-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_commit("commit-wait-2", std::make_pair(test_xpath, "true"));
    CHECK(r2.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r2.status.error_message(), Contains("commit already in progress"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: plain Set while waiting", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("commit-plain-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_plain_set(test_xpath, "true");
    CHECK(r2.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r2.status.error_message(), Contains("previous Set has to be confirmed"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit with zero rollback duration",
                 "[commit-neg]")
{
    auto r = send_commit_with_rollback("commit-neg-dur-1", 0, std::make_pair(test_xpath, "false"));
    CHECK(r.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r.status.error_message(), Contains("rollback_duration must be greater than 0"));
    check_error_response(r);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: commit with negative rollback duration",
                 "[commit-neg]")
{
    auto r = send_commit_with_rollback("commit-neg-dur-1", -1, std::make_pair(test_xpath, "false"));
    CHECK(r.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r.status.error_message(), Contains("rollback_duration must be greater than 0"));
    check_error_response(r);
}

// https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-commit-confirmed.md#322-confirm

TEST_CASE_METHOD(CommitFixture, "Commit extension: id mismatch on confirm", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("mismatch-A", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_confirm("mismatch-B");
    CHECK(r2.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r2.status.error_message(), Contains("commit id mismatch"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: confirm while not waiting", "[commit-neg]")
{
    // empty id
    auto r = send_confirm("");
    CHECK(r.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r);

    // non-empty id
    auto r2 = send_confirm("no-commit-confirm");
    CHECK(r2.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r2.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: mutations ignored on confirm", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("mut-confirm-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_confirm("mut-confirm-1", std::make_pair(test_xpath, "true"));
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(!impl::Commit::get_singleton().get_wait_confirm());

    // mutation was ignored; config stays at the committed value
    check_value(sysrepo::Datastore::Running, test_xpath, "false");
}

// https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-commit-confirmed.md#323-cancel

TEST_CASE_METHOD(CommitFixture, "Commit extension: id mismatch on cancel", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 =
        send_commit_with_rollback("cancel-mismatch-A", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_cancel("cancel-mismatch-B");
    CHECK(r2.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r2.status.error_message(), Contains("commit id mismatch"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: cancel while not waiting", "[commit-neg]")
{
    // empty id
    auto r = send_cancel("");
    CHECK(r.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r);

    // non-empty id
    auto r2 = send_cancel("no-commit-cancel");
    CHECK(r2.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r2.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: mutations ignored on cancel", "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("mut-cancel-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_cancel("mut-cancel-1", std::make_pair(test_xpath, "false"));
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(!impl::Commit::get_singleton().get_wait_confirm());

    // cancel rolled back to original; mutation ignored
    check_value(sysrepo::Datastore::Running, test_xpath, "true");
}

// https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-commit-confirmed.md#324-set-rollback-duration

TEST_CASE_METHOD(CommitFixture, "Commit extension: id mismatch on set_rollback_duration",
                 "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("srd-mismatch-A", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_set_rollback_duration("srd-mismatch-B", 60);
    CHECK(r2.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r2.status.error_message(), Contains("commit id mismatch"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: set_rollback_duration while not waiting",
                 "[commit-neg]")
{
    // empty id
    auto r = send_set_rollback_duration("", 60);
    CHECK(r.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r);

    // non-empty id
    auto r2 = send_set_rollback_duration("no-commit-srd", 60);
    CHECK(r2.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK_THAT(r2.status.error_message(), Contains("not waiting for confirm"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: set_rollback_duration with duration 0",
                 "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("dur-zero-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_set_rollback_duration("dur-zero-1", 0);
    CHECK(r2.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r2.status.error_message(), Contains("rollback_duration must be greater than 0"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: set_rollback_duration with negative duration",
                 "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("dur-neg-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_set_rollback_duration("dur-neg-1", -1);
    CHECK(r2.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r2.status.error_message(), Contains("rollback_duration must be greater than 0"));
    check_error_response(r2);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: mutations ignored on set_rollback_duration",
                 "[commit-neg]")
{
    set_initial_config(test_xpath, "true");

    auto r1 = send_commit_with_rollback("mut-srd-1", 120, std::make_pair(test_xpath, "false"));
    CHECK(r1.status.ok());

    auto r2 = send_set_rollback_duration("mut-srd-1", 60, std::make_pair(test_xpath, "true"));
    CHECK(r2.status.ok());
    check_empty_success_response(r2);

    CHECK(impl::Commit::get_singleton().get_rollback_secs() == 60);

    // mutation was ignored; config stays at the committed value
    check_value(sysrepo::Datastore::Running, test_xpath, "false");
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: no action set", "[commit-neg]")
{
    auto r = send_no_action("no-action-1");
    CHECK(r.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK_THAT(r.status.error_message(), Contains("commit extension has no action"));
    check_error_response(r);
}

TEST_CASE_METHOD(CommitFixture, "Commit extension: unsupported extension rejected", "[commit-neg]")
{
    auto r = send_unsupported_extension();
    CHECK(r.status.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    CHECK_THAT(r.status.error_message(), Contains("extension not supported"));
    check_error_response(r);
}
