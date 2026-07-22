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
#include "encode/encode.h"
#include <proto/gnmi.grpc.pb.h>
#include <proto/gnmi_ext.pb.h>
#include <sysrepo-cpp/utils/exception.hpp>
#include <sysrepo.h>
#include <utils/log.h>
#include <utils/sysrepo.h>
#include <utils/utils.h>

namespace impl
{

grpc::Status Set::handleUpdate(gnmi::Update in, gnmi::UpdateResult *out, std::string prefix_str,
                               const gnmi::Path &prefix, std::string op)
{
    // Parse request
    if (!in.has_path() || !in.has_val())
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Update no path or value");

    std::string fullpath;
    try
    {
        gnmi_check_origin(prefix, in.path());
        if (prefix.elem_size() > 0)
        {
            fullpath += prefix_str;
        }
        fullpath += gnmi_to_xpath(in.path());
    }
    catch (std::invalid_argument &exc)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
    }
    SLOG_DEBUG("Update (", op, ") ", fullpath);

    if (fullpath.compare("/*") != 0 && op.compare("replace") == 0)
    {
        // Check if the xpath we are replacing is a leaf-list or a list
        auto node_type = sr_sess.getContext().findPath(fullpath).nodeType();
        if (node_type == libyang::NodeType::Leaflist || node_type == libyang::NodeType::List)
        {
            // Replacing list or leaflist means we should delete all previous entries
            auto created_nodes = sr_sess.getContext().newPath2(fullpath, std::nullopt,
                                                               libyang::CreationOptions::Opaque);
            auto del_node = created_nodes.createdNode;
            auto sr_mod = sr_sess.getContext().getModuleImplemented("sysrepo").value();

            auto parent = del_node;
            auto check = del_node->parent();
            while (check.has_value())
            {
                parent = check;
                check->newMeta(sr_mod, "sysrepo:operation", "ether");
                check = check->parent();
            }

            if (del_node->isOpaque())
            {
                del_node->newAttrOpaqueJSON("sysrepo", "operation", "purge");
            }
            else
            {
                // libyang treats NULL as a valid value for some data types
                del_node->newMeta(sr_mod, "sysrepo:operation", "purge");
            }

            xact.merge(purgeTree, parent);
        }
    }

    auto [status, top_level] = encodef->decode(fullpath, in.val(), EncodePurpose::Set);

    if (!status.ok())
        return status;

    auto ietf_nc_mod = sr_sess.getContext().getModuleImplemented("ietf-netconf").value();
    if (fullpath.compare("/*") == 0)
    {
        if (op.compare("replace") == 0)
        {
            // The gNMI semantics are that a replace at the top-level should cause all data node not
            // provided to be removed. However, sysrepo semantics are that only the provided nodes
            // are replaced. Therefore, request that everything not being replaced is deleted.

            auto del_root = sr_sess.getData(fullpath.c_str(), 1);
            // Walk all siblings not in update and add delete node to them
            for (auto n = std::optional<libyang::DataNode>(del_root); n.has_value();
                 n = n->nextSibling())
            {
                // Default flags are unreliable since getData() retrieves only a subset of the
                // datastore
                // if (getRawNode(*n)->flags & LYD_DEFAULT)
                // {
                //     // Default nodes need not be deleted and can be skipped
                //     continue;
                // }

                bool is_replace_node = false;
                // Is this node a replace node?
                for (auto repl_n = top_level; repl_n.has_value(); repl_n = repl_n->nextSibling())
                {
                    if (n->schema().path() == repl_n->schema().path())
                    {
                        is_replace_node = true;
                        break;
                    }
                }

                // If this is a replace node, then optimise further sysrepo processing by not adding
                // it to the batch
                if (!is_replace_node)
                {
                    n->newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");
                    xact.merge(deleteTree, n);
                }
            }
        }

        // Add operation attribute to each node - there can be multiple if the JSON contains
        // multiple top-level nodes.
        for (auto n = top_level; n.has_value(); n = n->nextSibling())
        {
            n->newMeta(ietf_nc_mod, "ietf-netconf:operation", op);
        }

        if (op.compare("replace") == 0)
        {
            xact.merge(replaceTree, top_level);
        }
        else if (op.compare("merge") == 0)
        {
            xact.merge(updateTree, top_level);
        }
    }
    else
    {
        // Find the edit point for the data fragment
        auto set = top_level->findXPath(fullpath.c_str());
        // We should have found a path, and wildcards don't make sense
        if (set.empty())
        {
            SLOG_ERROR("Empty result searching for ", fullpath.c_str());
            throw std::invalid_argument("Invalid set returned for xpath \"" + fullpath + "\"");
        }

        for (auto edit_node : set)
        {
            edit_node.newMeta(ietf_nc_mod, "ietf-netconf:operation", op);
            SLOG_DEBUG(op.c_str(), " path: ", edit_node.path());
        }

        if (op.compare("replace") == 0)
        {
            xact.merge(replaceTree, top_level);
        }
        else if (op.compare("merge") == 0)
        {
            xact.merge(updateTree, top_level);
        }
    }

    // Fill in Response
    out->set_allocated_path(in.release_path());

    return status;
}

grpc::Status Set::run(const gnmi::SetRequest *request, gnmi::SetResponse *response)
{
    std::string prefix = "";
    std::vector<gnmi::UpdateResult> results;
    auto ietf_nc_mod = sr_sess.getContext().getModuleImplemented("ietf-netconf").value();

    // scan for Commit extension, allow only the Commit extension; reject all others
    const gnmi_ext::Commit *commit_ext = nullptr;
    for (const auto &ext : request->extension())
    {
        if (ext.has_commit())
            commit_ext = &ext.commit();
        else
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "extension not supported");
    }

    // dispatch on Commit extension action
    // kConfirm/kCancel/kSetRollbackDuration return early (no mutations applied)
    // kCommit arms the timer and falls through to apply mutations as a normal Set
    if (commit_ext)
    {
        switch (commit_ext->action_case())
        {
        case gnmi_ext::Commit::kCommit:
        {
            int64_t rollback_secs = Commit::default_rollback_secs;
            if (commit_ext->commit().has_rollback_duration())
                rollback_secs = commit_ext->commit().rollback_duration().seconds();
            auto st = commit_state->request_setup(commit_ext->id(), rollback_secs);
            if (!st.ok())
                return st;
            break; // proceed to apply mutations as a normal Set
        }
        case gnmi_ext::Commit::kConfirm:
        {
            auto st = commit_state->confirm(commit_ext->id());
            if (!st.ok())
                return st;
            response->set_timestamp(get_time_nanosec());
            return grpc::Status::OK;
        }
        case gnmi_ext::Commit::kCancel:
        {
            auto st = commit_state->cancel(commit_ext->id());
            if (!st.ok())
                return st;
            response->set_timestamp(get_time_nanosec());
            return grpc::Status::OK;
        }
        case gnmi_ext::Commit::kSetRollbackDuration:
        {
            int64_t rollback_secs = 0;
            if (commit_ext->set_rollback_duration().has_rollback_duration())
                rollback_secs = commit_ext->set_rollback_duration().rollback_duration().seconds();
            auto st = commit_state->set_rollback_duration(commit_ext->id(), rollback_secs);
            if (!st.ok())
                return st;
            response->set_timestamp(get_time_nanosec());
            return grpc::Status::OK;
        }
        case gnmi_ext::Commit::ACTION_NOT_SET:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "commit extension has no action");
        }
    }

    // a plain Set while a rollback counter is running
    // must fail with FAILED_PRECONDITION
    if (!commit_ext && commit_state->get_wait_confirm())
    {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "previous Set has to be confirmed");
    }

    response->set_timestamp(get_time_nanosec());

    /* Prefix for gNMI path */
    if (request->has_prefix())
    {
        try
        {
            prefix = gnmi_to_xpath(request->prefix());
        }
        catch (std::invalid_argument &exc)
        {
            SLOG_ERROR(exc.what());
            commit_state->clear();
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
        }
        SLOG_DEBUG("prefix is ", prefix);
        response->mutable_prefix()->CopyFrom(request->prefix());
    }

    /* gNMI paths to delete */
    if (request->delete__size() > 0)
    {
        // sort the paths to delete in order from parent to child (so that operations are
        // successfuly merged)
        std::set<std::string, std::less<std::string>> del_paths;
        for (auto delpath : request->delete_())
        {
            // Parse request and config sysrepo
            std::string fullpath;
            try
            {
                gnmi_check_origin(request->prefix(), delpath);
                fullpath = prefix + gnmi_to_xpath(delpath);
                del_paths.insert(fullpath);
            }
            catch (std::invalid_argument &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
            }

            // Fill in Response
            gnmi::UpdateResult res;
            *(res.mutable_path()) = delpath;
            res.set_op(gnmi::UpdateResult::DELETE);
            results.push_back(res);
        }

        for (auto &fullpath : del_paths)
        {
            SLOG_DEBUG("Delete ", fullpath);
            try
            {
                // We cannot use deleteItem here as sysrepo doesn't like it being
                // mixed with edit_batch, so retrieve just the nodes referenced by
                // the xpath and no deeper to avoid it being any more expensive
                // than it has to be
                auto del_root = sr_sess.getData(fullpath, 0);
                if (!del_root.has_value())
                    throw std::invalid_argument("xpath \"" + fullpath + "\" not found");

                if (fullpath.compare("/*") == 0)
                {
                    // Walk all siblings and add delete node to them
                    for (auto n : del_root->siblings())
                    {
                        n.newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");

                        SLOG_DEBUG(" 1. Delete path: ", n.path());
                    }
                }
                else
                {
                    // Find the node(s) actually referenced by the path to mark them as
                    // requiring delete since they could well be deeper than the root
                    // node
                    auto set = del_root->findXPath(fullpath.c_str());
                    if (set.empty())
                        throw std::invalid_argument("xpath \"" + fullpath + "\" not found");

                    auto sr_mod = sr_sess.getContext().getModuleImplemented("sysrepo").value();
                    for (auto n : set)
                    {
                        // Ensure we don't create any parent nodes - they might be deleted
                        // in this transaction.
                        auto p = n.parent();
                        while (p.has_value())
                        {
                            p->newMeta(sr_mod, "sysrepo:operation", "ether");
                            p = p->parent();
                        }

                        n.newMeta(ietf_nc_mod, "ietf-netconf:operation", "remove");
                        SLOG_DEBUG(" 2. Delete path: ", n.path());
                    }
                }
                xact.merge(deleteTree, del_root);
            }
            catch (const std::invalid_argument &exc)
            {
                SLOG_ERROR(exc.what());
                // gNMI spec §3.4.6: In the case that a path specifies an element within the data
                // tree that does not exist, these deletes MUST be silently accepted.
            }
            catch (const std::exception &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
            }
        }
    }

    /* gNMI paths with value to replace */
    if (request->replace_size() > 0)
    {
        for (auto &upd : request->replace())
        {
            gnmi::UpdateResult res;
            try
            {
                auto status = handleUpdate(upd, &res, prefix, request->prefix(), "replace");
                if (!status.ok())
                {
                    SLOG_ERROR("Fail building set notification: ", status.error_message());
                    commit_state->clear();
                    return status;
                }

                res.set_op(gnmi::UpdateResult::REPLACE);
                results.push_back(res);
            }
            catch (const std::invalid_argument &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
            }
            catch (sysrepo::Error &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
            }
            catch (const std::exception &exc)
            { // Any other exception
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
            }
        }
    }

    /* gNMI paths with union_replace -- not yet supported */
    if (request->union_replace_size() > 0)
    {
        SLOG_WARN("Unsupported union_replace in SetRequest");
        commit_state->clear();
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "union_replace not supported");
    }

    /* gNMI paths with value to update */
    if (request->update_size() > 0)
    {
        for (auto &upd : request->update())
        {
            gnmi::UpdateResult res;
            try
            {
                auto status = handleUpdate(upd, &res, prefix, request->prefix(), "merge");
                if (!status.ok())
                {
                    SLOG_ERROR("Fail building set notification: ", status.error_message());
                    commit_state->clear();
                    return status;
                }
                res.set_op(gnmi::UpdateResult::UPDATE);
                results.push_back(res);
            }
            catch (const std::invalid_argument &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, exc.what());
            }
            catch (const sysrepo::Error &exc)
            {
                SLOG_ERROR(exc.what());
                commit_state->clear();
                return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
            }
        }
    }

    xact.push(deleteTree);
    xact.push(purgeTree);
    xact.push(replaceTree);
    xact.push(updateTree);

    try
    {
        /* edit final tree batch */
        if (xact.final_tree.has_value())
            sr_sess.editBatch(xact.final_tree.value(), sysrepo::DefaultOperation::Merge);

        /* if this fails, we can still revert the changes */
        sr_sess.applyChanges();

        if (!commit_ext)
        {
            /* copy the prepared configuration to Startup (has to succeed) */
            sr_sess_startup.copyConfig(sysrepo::Datastore::Running);
        }
    }
    catch (const sysrepo::Error &exc)
    {
        commit_state->clear();
        std::string err_str;
        auto errors = sr_sess.getErrors();
        if (errors.size())
        {
            err_str = errors[0].errorMessage;
        }
        else
        {
            err_str = exc.what();
        }
        SLOG_ERROR("commit error: ", err_str);
        sr_sess.discardChanges();
        return grpc::Status(grpc::StatusCode::ABORTED, err_str);
    }
    catch (const std::exception &exc)
    {
        commit_state->clear();
        SLOG_ERROR(exc.what());
        sr_sess.discardChanges();
        return grpc::Status(grpc::StatusCode::INTERNAL, exc.what());
    }

    for (auto r : results)
        *(response->add_response()) = r;

    // start the confirm timer (timeout callback)
    if (commit_ext)
    {
        commit_state->request_finish();
    }
    return grpc::Status::OK;
}

} // namespace impl
