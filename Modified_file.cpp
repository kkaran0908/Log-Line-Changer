#include "adl_algo_bin.hpp"

#include <algoif/types.h>
#include <algoif/blockif.h>
#include <algoif/algoinstif.h>
#include <algoif/dlif.h>
#include <orderd/group_ids.h>
#include "orderd/group_ids.hpp"
#include "server_connection.hpp"
#include <ttsdk/internal/logger.hpp>
#include <ttsdk/internal/algoinstmgr.hpp>
#include "host_exec_if.hpp"
#include "events.hpp"
#include "algo_bin.hpp"
#include <orderutil/submitter.hpp>  //  needed for tt::orderutil::MarshallClassOneTwoFields

using namespace algojob;
using namespace algojob::adl;
using namespace algoif;
using namespace tt::sdk::internal;
using namespace tt::algoutil;

void adl_algo_bin::process_external_request (const std::shared_ptr<AlgoJobRequestData>& data)
{
    ScopeLogger<> scope_process_external_request("process_external_request");
    auto& request = data->request;

    if (   m_algo_inst_mgr != nullptr
        && TTSDK_PARAM_IS_SET(&request->params, current_user_id)
        && request->params.current_user_id != m_algo_params.current_user_id)
    {
        m_algo_inst_mgr->OnCurrentUserIdChanged(request->params.current_user_id);
    }

    check_class_2_fields(data);

    //Dont waste time merging incoming request param if it is:
    if (   request->id != algojob__request_export_value_snapshot
        && request->id != algojob__request_register_side_channel
        && request->id != algojob__request_unregister_side_channel
        && request->id != algojob__request_handle_user_disconnect
        && request->id != algojob__requets_server_disconnected
        && request->id != algojob__requets_server_reconnected
        && request->id != algojob__request_order_fill_update
        && request->id != algojob__request_order_pass)
    {
        TTSDK_MergeParams(CastIntParamsToExt(&request->params), CastIntParamsToExt(&m_algo_params));
    }


    auto state = m_state.load(std::memory_order_relaxed);
    TTLOG<<"        [algo:"<< inst_id().to_string() <<"]: process_external_request=["<< ALGOJOB_REQUEST_ID_STR[request->id] <<"], state="<< ALGO_STATE_STR[state] <<;

    switch(request->id)
    {
        case algojob__request_recovery:
        {
            // Check if ADL Recovery is enabled on this server.
            if (!env::instance().is_adl_recovery_enabled())
            {
                TTLOG<<"[algo:"<< inst_id().to_string() <<"]: ADL Recovery is not supported - setting algo_state to [failed] and cleaning up child orders";
                m_state.store(algojob__algo_failed, std::memory_order_relaxed);
                TTSDKInt_CleanupChildOrder(&request->params.ord_id, false);
                return server_connection::instance().send_request_failure(
                    request->id,
                    request->flags,
                    request->user_request_id,
                    algojob__algo_failed,
                    algojob__request_failure_can_not_process,
                    inst_id(),
                    &request->params
                );
                return;
            }
            // If ADL Recovery is enabled, send class_1_2_fields to OrderD and then fall through to "algojob__request_deploy_instr" case below.
            tt::orderutil::MarshallClassOneTwoFields(inst_id(), m_class_1_2_fields);
        }
        case algojob__request_deploy_instr:
        {
            auto is_new = request->id == algojob__request_deploy_instr ? true : false;

            std::string scope_logger_identifier = is_new ? "process_external_request::algojob__request_deploy_instr" :
                                                           "process_external_request::algojob__request_recovery";
            ScopeLogger<> scope_logger(scope_logger_identifier);
            // Verify that _algo_version is set
            const char *algo_version = "";
            TTSDK_GetAlgoVersion(CastIntParamsToExt(&request->params), &algo_version);

            if (algo_version[0] == '\0')
            {
                // Algo version is missing
                TTLOG<<"[algo:"<< inst_id().to_string() <<"] Request is missing algo version.";

                TTSDK_SetMessage(
                    CastIntParamsToExt(&request->params),
                    "Required parameter \"_algo_version\" missing"
                );

                return server_connection::instance().send_request_failure(
                    request->id,
                    request->flags,
                    request->user_request_id,
                    algojob__algo_failed,
                    algojob__request_failure_init_failure,
                    inst_id(),
                    &request->params
                );
            }

            // Epiq Flag
            bool epiqFlag{false};
            TTSDKInt_GetEpiqFlag(CastIntParamsToExt(&request->params), &epiqFlag);
            TTLOG<<"[algo:"<< inst_id().to_string() <<"] EPIQ_flag="<< epiqFlag <<;

            auto algo_instr_id = is_new ? request->data.deploy_instr.instr_id : request->data.recovery.instr_id;
            algo_dl* dl = ref_algo_dl(algo_instr_id,
                                      algo_version,
                                      request->params.current_user_id,
                                      true);

            auto reason = is_new? instr_compile_new: instr_compile_recovered;
            if (dl)
            {
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    m_dl = dl;
                }

                return dispatch_service_event(std::make_shared<instr_download_service_event>(reason));
            }

            return dispatch_service_event(
                std::make_shared<instr_compilation_service_event>(algo_instr_id, algo_version, reason)
            );
        }

        case algojob__request_deploy_def:
            return dispatch_service_event(
                std::make_shared<def_compilation_service_event>(
                    request->data.deploy_def.root_def,
                    static_cast<AlgoJobDeployDefRequestData*>(data.get())->def
                )
            );

        case algojob__request_start:
            // Initiate start is handled in ProcessCompilationCompleteEvent
            return reject_request(request);

        case algojob__request_resume:
            if (state == algojob__algo_paused)
            {
                set_pending_data(request);
                init_action(algoActionResume);
            }
            else
            {
                return reject_request(request);
            }
            break;

        case algojob__request_pause:
            if (state == algojob__algo_started)
            {
                set_pending_data(request);
                init_action(algoif::algoActionPause);
            }
            else
            {
                return reject_request(request);
            }
            break;

        case algojob__request_stop:
            switch(state)
            {
                case algojob__algo_started:
                case algojob__algo_paused:
                    set_pending_data(request);
                    init_action(algoif::algoActionStop);
                    break;

                case algojob__algo_compiling:
                    // Set the state as stopping, we will check for this flag in
                    // ProcessCompilationCompleteEvent and ProcessCompilationFailedEvent
                    m_state.store(algojob__algo_stopping, std::memory_order_relaxed);
                    break;

                case algojob__algo_failed:
                case algojob__algo_compiling_failed:
                case algojob__algo_recovery_failed:
                    // Detach All Child Orders before deleting the algo_inst.

                    // DEB-48497 when compiling failed, the m_algo_inst_mgr is null
                    if (m_algo_inst_mgr)
                    {
                        m_algo_inst_mgr->DetachChildOrders();
                    }
                    // Send back the Cancel OK
                    server_connection::instance().send_request_complete(
                        request->id,
                        request->flags,
                        request->user_request_id,
                        algojob__algo_stopped,
                        inst_id(),
                        &m_algo_params
                    );

                    schedule_destroy();
                    return;

                case algojob__algo_starting:
                case algojob__algo_resuming:
                case algojob__algo_pausing:
                case algojob__algo_updating_paused:
                case algojob__algo_updating_started:
                case algojob__algo_recovering:
                    // clear pending actions
                    m_host_if->ClearPendingAction();

                    set_pending_data(request);
                    init_action(algoif::algoActionStop);
                    return;

                case algojob__algo_stopping:
                case algojob__algo_detaching_for_stopped:
                    // Alog stop already processed, send back the Cancel OK
                    server_connection::instance().send_request_complete(
                        request->id,
                        request->flags,
                        request->user_request_id,
                        algojob__algo_stopping,
                        inst_id(),
                        &m_algo_params
                    );
                    return;

                default:
                    return reject_request(request);
            }

            break;

        case algojob__request_update:
            if (state == algojob__algo_started ||
                state == algojob__algo_paused)
            {
                set_pending_data(request);
                init_action(algoif::algoActionUpdate);
            }
            else
            {
                TTSDK_SetExecRestatementReason(CastIntParamsToExt(&request->params), ttsdk_exec_restatement_reason_replace_rejected);
                return reject_request(request);
            }
            break;

        case algojob__request_register_side_channel:
            if (m_inst_if == nullptr || m_inst_if->HasExportValue())
            {
                m_exec_flags = (algojob__exec_flags)(m_exec_flags|algojob__exec_export_values);
                server_connection::instance().send_side_channel_response(
                    algojob__response_register_side_channel,
                    request->user_request_id,
                    inst_id(),
                    side_channel__request_completed
                );
            }
            else
            {
                ALGO_WLOG("[algo:%s] Algo has no Export Value. Ignore reuqest=%s", inst_id().to_string(),ALGOJOB_REQUEST_ID_STR[request->id]);
            }
            return;

        case algojob__request_unregister_side_channel:
            m_exec_flags = (algojob__exec_flags)(m_exec_flags & ~algojob__exec_export_values);
            server_connection::instance().send_side_channel_response(
                algojob__response_unregister_side_channel,
                request->user_request_id,
                inst_id(),
                side_channel__request_completed
            );
            return;

        case algojob__request_export_value_snapshot:
            if (m_inst_if == nullptr || m_inst_if->HasExportValue())
            {
                m_exec_flags = (algojob__exec_flags)(m_exec_flags | algojob__exec_export_values);
                if (m_inst_if != nullptr)
                {
                    server_connection::instance().send_side_channel_response(
                        algojob__response_export_value_snapshot,
                        request->user_request_id,
                        inst_id(),
                        side_channel__request_completed
                    );
                    try_collect_and_send_values(true);
                }
            }
            else
            {
                TTLOG<<"Algo has no Export Value. Ignore reuqest="<< ALGOJOB_REQUEST_ID_STR[request->id] <<;
            }
            return;
        case algojob__requets_server_disconnected:
            TTLOG<<"[algo:"<< inst_id().to_string() <<"] Detected server disconnect but will ignore it!";
            return;
        case algojob__request_handle_user_disconnect:
            {
                if (state == algojob__algo_compiling)
                {
                    // the algo instance is not fully created yet when user disconnect
                    // set the state to stopping, it will be handled after
                    // algo extracting/compiling finished
                    m_state.store(algojob__algo_stopping, std::memory_order_relaxed);
                }
                else if (m_inst_if != nullptr &&
                         m_host_if != nullptr &&
                         m_root_block_graph != nullptr) // Ensure the ADL algo is fully-formed before executing Pause or Stop
                {
                    enum ttsdk_user_disconnect_action action = leave_on_disconn;
                    TTSDKInt_GetUserDisconnectAction(CastIntParamsToExt(&m_algo_params), &action);
                    std::string msg = request->id == algojob__request_handle_user_disconnect
                                        ? "User disconnected"
                                        : "AlgoServer disconnected";
                    switch (action)
                    {
                        case pause_on_disconn:
                            m_host_if->PauseAlgo(msg);
                            break;
                        case cancel_on_disconn:
                            m_host_if->StopAlgo(msg);
                            break;
                        case leave_on_disconn:
                            break;
                    }
                }
            }
            return;
        case algojob__requets_server_reconnected:
            //Already handled in request_receiver thread.
            return;
        case algojob__request_set_oma_parent_id:
            if (state != algojob__algo_started &&
                state != algojob__algo_paused)
            {
                // On we reject, we must clear the OMA Parent ID because new OMA Parent ID was already
                // merged into m_algo_params at the beginning of this function.
                // This assumes that, before the merge, this algo instance had no OMA Parent ID.
                TTSDK_ClearOMAParentOrderId(CastIntParamsToExt(&m_algo_params));
                return reject_request(request);
            }

            // Nothing to do on accept since new OMA Parent ID was already merged into m_algo_params.
            char oma_parent_uuid_string[TTSDK_MAX_UUID_STRING_SIZE];
            memset(oma_parent_uuid_string, 0, TTSDK_MAX_UUID_STRING_SIZE);
            TTSDKInt_GetOMAParentIdString(CastIntParamsToExt(&m_algo_params), &oma_parent_uuid_string);
            TTLOG<<"                      [algo:"<< inst_id().to_string() <<"] Updated OMA Parent ID on request: ";
            server_connection::instance().send_request_complete(
                request->id,
                request->flags,
                request->user_request_id,
                state,
                inst_id(),
                &m_algo_params
            );
            return;
        case algojob__request_clear_oma_parent_id:
            if (state != algojob__algo_started &&
                state != algojob__algo_paused)
            {
                return reject_request(request);
            }

            // Clear OMA Parent ID from m_algo_params.
            TTSDK_ClearOMAParentOrderId(CastIntParamsToExt(&m_algo_params));
            TTLOG<<"                      [algo:"<< inst_id().to_string() <<"] Cleared OMA Parent ID on request: ";
            server_connection::instance().send_request_complete(
                request->id,
                request->flags,
                request->user_request_id,
                state,
                inst_id(),
                &m_algo_params
            );
            return;
        case algojob__request_order_fill_update:
            //Already handled in check_class_2_fields
            return;
        case algojob__request_order_pass:
        {
            ttsdk_params_internal* request_params = &request->params;
            if (request_params == nullptr)
            {
                TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: request_params was a nullptr";
                return reject_request(request);
            }

            enum order_pass obp_state = order_pass_none;
            TTSDKInt_GetOrderPassingState(CastIntParamsToExt(request_params), &obp_state);

            unsigned long long current_group_id = 0;
            TTSDKInt_GetCurrentGroupId(CastIntParamsToExt(request_params), &current_group_id);

            unsigned long long pass_to_group_id = 0;
            TTSDKInt_GetPassToGroupId(CastIntParamsToExt(request_params), &pass_to_group_id);

            unsigned long long previous_pass_to_group_id = 0;
            TTSDKInt_GetPrevPassToGroupId(CastIntParamsToExt(request_params), &previous_pass_to_group_id);

            unsigned long long original_group_id = 0;
            TTSDKInt_GetOriginalGroupId(CastIntParamsToExt(request_params), &original_group_id);

            TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Attempting to update params with: ";

            // Perform validations.
            if (state != algojob__algo_started &&
                state != algojob__algo_paused)
            {
                TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Not allowed in current state="<< std::string(ALGO_STATE_STR[state]) <<;
                return reject_request(request);
            }

            if (obp_state == order_pass_none)
            {
                TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Missing OBP state!";
                return reject_request(request);
            }

            if (obp_state == order_pass_accept || obp_state == order_pass_set_child)
            {
                if (current_group_id == 0)
                {
                    TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Missing current_group_id for OBP state="<< std::string(OrderPassToString(obp_state)) <<"!";
                    return reject_request(request);
                }

                if (m_algo_inst_mgr == nullptr)
                {
                    TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Missing AlgoInstMgr for OBP state="<< std::string(OrderPassToString(obp_state)) <<"!";
                    return reject_request(request);
                }
            }

            // All validations passed - we can update m_algo_params now.
            TTSDK_MergeParams(CastIntParamsToExt(request_params), CastIntParamsToExt(&m_algo_params));

            // For "order_pass_accept" and "order_pass_set_child," set current_group_id for current/future child orders.
            // Earlier above, we ensured that current_group_id and m_algo_inst_mgr are valid.
            if (obp_state == order_pass_accept || obp_state == order_pass_set_child)
            {
                group__ids group_ids{ current_group_id, original_group_id, pass_to_group_id, previous_pass_to_group_id };
                TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Set current and future child orders with "<< GroupIDsToString(group_ids) <<;
                m_algo_inst_mgr->OnCurrentUserGroupIdChanged(group_ids);
            }

            TTLOG<<"[algo:"<< inst_id().to_string() <<"] algojob__request_order_pass: Success!";
            server_connection::instance().send_request_complete(
                request->id,
                request->flags,
                request->user_request_id,
                state,
                inst_id(),
                &m_algo_params
            );
            return;
        }
        default:
            return reject_request(request);
    }
}

void adl_algo_bin::init_action (algoif::AlgoAction action)
{
    if (m_host_if->IsActionValid(action) == false)
    {
        ALGO_WLOG("[algo:%s] action=%s invalid: state=%s",
                  inst_id().to_string(),
                  ALGO_ACTION_STR[ action],
                  ALGO_STATE_STR[m_host_if->get_state()]);
        return;
    }

    if (m_host_if->HasPendingAction())
    {
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] queue up action="<< ALGO_ACTION_STR[action] <<". state="<< ALGO_STATE_STR[m_host_if->get_state()] <<;
        m_host_if->PushPendingAction(action);
        return;
    }

    m_host_if->PushPendingAction(action);
    m_host_if->InternalInitiateAction(action);
}

void adl_algo_bin::process_host_event (const std::shared_ptr<void>& data)
{
    switch(static_cast<host_event*>(data.get())->id)
    {
    case host_event_id::compilation_failed:
        {
            auto p=static_cast<compilation_failed_host_event*>(data.get());
            process_compilation_failed_event(p->failure, p->m_error);
            break;
        }


        case host_event_id::compilation_complete:
            process_compilation_completed_event(static_cast<compilation_complete_host_event*>(data.get())->compile_reason);
            break;

        case host_event_id::signal_connector:
        {
            auto event = static_cast<signal_connector_host_event*>(data.get());
            // Block can mute its outputs (even after scheduling them to signal) if the blockGraph to which it belongs needs to be exited.
            if (event->block_exec_if->OutputsMuted() == true)
            {
                break;
            }

            // Suppress signal if the algo instance is pausing, unless explicitly specified to ignore algo instance state.
            auto state = m_state.load(std::memory_order_relaxed);
            if (event->drop_on_pause == true &&
                (m_state == algojob__algo_pausing ||
                 m_state == algojob__algo_detaching_for_paused ||
                 m_state == algojob__algo_paused ||
                 m_state == algojob__algo_stopping ||
                 m_state == algojob__algo_detaching_for_stopped ||
                 m_state == algojob__algo_stopped))
            {
                break;
            }

            if (m_host_if->IsStoppedSignaler(event->signaler_id))
            {
                 TTLOG<<"[algo:"<< inst_id().to_string() <<"] signaler_id="<< event->signaler_id <<"lu is stopped, dropped this event";
            }
            else
            {
                const algoif::BlockExecIf* block_exec_if = event->block_exec_if;
                const algoif::BlockVertex* block_vertex = block_exec_if->GetBlockVertex();
                m_inst_if->SignalConnector(event->connector_vertex, tt::algoutil::AlgoEventInfo(tt::algoutil::AlgoEventType::signal_connector, block_vertex)); // ProcessActionBlocks() called inside SignalConnector()
                run_periodic_host_task();
            }
            break;
        }

        case host_event_id::exit_block_graph:
        {
            auto event = static_cast<exit_block_graph_host_event*>(data.get());
            TTLOG<<"[algo:"<< inst_id().to_string() <<"] now process scheduled exit_block_graph m_globalVIndex="<< event->m_globalVIndex <<"lu";
            m_inst_if->ExitBlockGraph(event->block_graph, false);
            break;
        }

        case host_event_id::perform_action:
            init_action(static_cast<perform_action_host_event*>(data.get())->action);
            break;

        case host_event_id::block_event:
        {
            auto event = static_cast<block_action_host_event*>(data.get());
            (*event->event_function)(event->block_exec_if);
            break;
        }

        case host_event_id::action_failed:
            process_action_failed_event();
            break;

        case host_event_id::action_complete:
            process_action_complete_event();
            break;
        case host_event_id::destroy_algo_inst:
            immediate_destroy();
            break;
    }
}

void adl_algo_bin::process_compilation_failed_event (algojob__request_failure_code failure, const std::string& error_msg)
{
    ScopeLogger<> scope_process_compilation_failed_event("process_compilation_failed_event");
    if(m_state.load(std::memory_order_relaxed) == algojob__algo_stopping)
    {
        // Algo was stopped while compiling
        // Send back the Cancel OK
        server_connection::instance().send_request_complete(
            m_pending_request,
            algojob__algo_stopped,
            inst_id(),
            &m_algo_params
        );

        //it is possible there is price subscription made and price event queued already
        //so destroy async.
        schedule_destroy();
    }
    else
    {
        if(failure != algojob__request_failure_none)
        {
            const char* current_msg = "";
            if(TTSDK_GetMessage(CastIntParamsToExt(&m_algo_params), &current_msg) == ttsdk_error_param_not_set)
            {
                TTSDK_SetMessage(CastIntParamsToExt(&m_algo_params),
                                 error_msg.empty()?ALGO_REQ_FAILURE_CODE_STR[failure]:error_msg.c_str());
            }
        }
        m_state.store(algojob__algo_compiling_failed, std::memory_order_relaxed);

        send_status();
    }
}

void adl_algo_bin::process_compilation_completed_event (enum instr_compile_reason reason)
{
    auto state = m_state.load(std::memory_order_relaxed);
    TTLOG<<"[algo:"<< inst_id().to_string() <<"]: process_compilation_completed_event: ";

    ScopeLogger<> scope_process_compilation_completed_event("process_compilation_completed_event");
    std::string error;
    try
    {
        if (state == algojob__algo_stopping)
        {
            // Algo was stopped while compiling.

            // Send back the Cancel OK.
            server_connection::instance().send_request_complete(
                m_pending_request,
                algojob__algo_stopped,
                inst_id(),
                &m_algo_params
            );

            // It is possible there was a price subscription made and a price update has been queued already.
            // So queue the destruction to flush the queued price updates instead of destroying immediately.
            schedule_destroy();
            return;
        }

        // Perform validations related to TT Analytics Block.
        TTLOG<<"[algo:"<< inst_id().to_string() <<"]: hasAnalytics="<< (dl()->algo_graph.analyticsBlockCount>0) <<" analyticsBlockCount="<< //keepingthebooleanhasAnalyticsforlog-levelcompatibilitydl()->algo_graph.analyticsBlockCount <<;

        // Check if the analytics block is enabled on this server.
        if (!env::instance().is_analytics_enabled() && dl()->algo_graph.analyticsBlockCount > 0)
        {
            ALGO_WLOG("[algo:%s]: Analytics Block usage is disabled on the server",
                      inst_id().to_string());
            throw algojob__request_failure_analytics_disabled;
        }

        // Check that the number of analytics blocks per algo does not exceed the limit.
        if (dl()->algo_graph.analyticsBlockCount > env::instance().max_analytics_blocks())
        {
            ALGO_WLOG("[algo:%s]: The number of Analytics Blocks in the algo (%d) exceeds the max allowed on the server (%d)",
                      inst_id().to_string(),
                      dl()->algo_graph.analyticsBlockCount,
                      env::instance().max_analytics_blocks());
            throw algojob__request_failure_too_many_analytics_blocks;
        }

        // In the sections that follow, we construct the following management objects for this algo instance.
        // 1. [algoif::host_exec_if] --(derived from)--> [algoif::HostExecIf]
        // 2. [artutils::AlgoInstIf] --(dervied from)--> [algoif::AlgoInstIf]
        //      --> Contains as a member: [artutils::AlgoInstExecIf] --(derived from)--> [algoif::HostExecIf]
        // 3. AlgoInstMgr

        // Construct: [algoif::host_exec_if] --(derived from)--> [algoif::HostExecIf]
        bool initPaused = m_exec_flags & algojob__exec_deploy_paused;
        m_host_if = std::make_unique<host_exec_if>(*this, initPaused);
        if (dl()->dl_if == nullptr)
        {
            throw std::runtime_error("[algo:" + inst_id().to_string()+ "] dl()->dl_if is NULL");
        }

        // Construct: [artutils::AlgoInstIf] --(dervied from)--> [algoif::AlgoInstIf]
        m_inst_if = dl()->dl_if->AllocAlgoInst(m_host_if.get());
        if(m_inst_if == nullptr)
        {
            throw std::runtime_error("[algo:" + inst_id().to_string()+ "]AllocAlgoInst failed");
        }

        // Ensure that future child order will be placed with correct curr_user_group_id.
        unsigned long long current_group_id = 0;
        TTSDKInt_GetCurrentGroupId(CastIntParamsToExt(&m_algo_params), &current_group_id);
        unsigned long long original_group_id = 0;
        TTSDKInt_GetOriginalGroupId(CastIntParamsToExt(&m_algo_params), &original_group_id);
        group__ids group_ids{ current_group_id, original_group_id, 0, 0 /*pass_to_group_id, previous_pass_to_group_id*/ };
        TTLOG<<"[algo:"<< inst_id().to_string() <<"]: Creating AlgoInstMgr with "<< GroupIDsToString(group_ids) <<;

        // Construct: AlgoInstMgr
        // Capture a shared_ptr of this object (adl_algo_bin) to ensure that this object
        // remains valid as long as m_algo_inst_mgr remains valid.
        auto ptr = m_algo_bin.shared_from_this();
        m_algo_inst_mgr = CreateAlgoInstMgr(
            inst_id(),
            m_algo_bin.user_id(),
            m_algo_bin.originating_user_id(),
            dl()->algo_compliance_id,
            ttsdk_algo_type_adl,
            [this, ptr](std::shared_ptr<void>&& data, tt::algoutil::AlgoEvent::EventFunc&& func, tt::algoutil::AlgoEventType event_type)
            {
                ptr->dispatch(std::move(data), std::move(func), event_type);
            },
            [this, ptr]()
            {
                return ptr->get_class_fields_copy();
            },
                dl()->instr_id,
                false, /* company_enable_near_far_touch_price_reasonability */
                false, /* company_enable_algo_risk_checks */
                group_ids,
                false);

        if (!m_algo_inst_mgr)
        {
            // Should never happen.
            process_compilation_failed_event(algojob__request_failure_init_failure, "Failed to CreateAlgoInstMgr");
            return;
        }

        if (reason == instr_compile_recovered)
        {
            // Download child orders before continuing with recovery process.
            // Download response will arrive in adl_algo_bin::on_children_recovered.
            TTLOG<<"[algo:"<< inst_id().to_string() <<"]: process_compilation_completed_event: ";
            recover_children();
            return;
        }

        // reason == instr_compile_new
        create_block_graph_and_init_action(reason);
    }
    catch (enum algojob__request_failure_code code)
    {
        process_compilation_failed_event(code, error);
    }
    catch (const std::runtime_error &e)
    {
        error = std::string(e.what());
        TTLOG<<<< error <<;
        process_compilation_failed_event(algojob__request_failure_init_failure, error);
    }
}

void
adl_algo_bin::create_block_graph_and_init_action (enum instr_compile_reason reason)
{
    auto state = m_state.load(std::memory_order_relaxed);
    TTLOG<<"[algo:"<< inst_id().to_string() <<"]: create_block_graph_and_init_action: ";

    // We must have constructed the following management objects earlier
    // in process_compilation_completed_event:
    // 1. [algoif::host_exec_if] --(derived from)--> [algoif::HostExecIf]
    // 2. [artutils::AlgoInstIf] --(dervied from)--> [algoif::AlgoInstIf]
    //      --> Contains as a member: [artutils::AlgoInstExecIf] --(derived from)--> [algoif::HostExecIf]
    // 3. AlgoInstMgr

    // This function delves into the algo logic itself and constructs
    // the algo runtime objects (eg. blocks) and puts the algo into motion.

    std::string error;
    try
    {
        // Subscribe AlgoInstMgr to PnL updates if the algo has PnL Block.
        m_algo_inst_mgr->m_hasPnl = dl()->algo_graph.hasPnL;
        if (m_algo_inst_mgr->m_hasPnl)
        {
            m_host_if->SubscribeProfitLossUpdate();
        }

        // Construct the algo graph (blocks and edges).
        auto root_def_block = m_inst_if->ReflectionData().RootDefinition();
        m_root_block_graph = std::make_unique<algoif::BlockGraph>();
        *(m_root_block_graph.get()) = m_inst_if->CreateBlockGraph(root_def_block,
                                                                  m_host_if->InitPaused(),
                                                                  CastConstIntParamsToExt(&m_algo_params));
        m_root_block_graph_set = true;

        // See if we need to setup a recurring periodic task to export values from the algo instance.
        // There are several things that an algo algo instance can opt in to export.
        // Option (a) is exclusive to AlgoServer DEBUG while rest are exclusive to AlgoServer EXEC.
        // (a) Export ALL Blocks' output connector: This option is necessary to render block values on ADLjs canvas.
        // (b) Export SELECTED Blocks' output connector: This option allows user to view selected block values on TTW widgets.
        // (c) Export Alert Blocks' Alert Messages: This option allows user to view Alert Messages on TTW widgets.
        if (m_exec_flags & algojob__exec_collect_values)
        {
            // We must be on AlgoServer DEBUG.
            m_has_periodic_task = true;
        }
        else
        {
            // DEB-43812: TTW can blindly send an unnecessary snapshot register request
            // which can cause "algojob__exec_export_values" flag to be incorrectly set.
            // We cannot trust TTW so let's check all blocks to see if any block really
            // wants to export its output value.
            if (m_inst_if->HasExportValue())
            {
                m_exec_flags = (enum algojob__exec_flags)(m_exec_flags | algojob__exec_export_values);
            }
            else
            {
                m_exec_flags = (enum algojob__exec_flags)(m_exec_flags & ~algojob__exec_export_values);
            }

            m_has_periodic_task = m_inst_if->HasExportValue() || dl()->algo_graph.hasAlert;
        }

        TTLOG<<"[algo:"<< inst_id().to_string() <<"]: m_has_periodic_task="<< m_has_periodic_task <<" HasExportValue="<< m_inst_if->HasExportValue() <<" hasAlert="<< dl()->algo_graph.hasAlert <<" hasPnL="<< m_algo_inst_mgr->m_hasPnl <<;

        m_algo_params.has_export_value = m_inst_if->HasExportValue()? 1 : 2; //use explict 2 to tell the difference btw No and dont_know

        // Earlier in CreateBlockGraph, we must have parsed all Instrument Blocks in the algo,
        // and collected all Accounts that the user wishes to use. Now we iterate through all
        // Accounts to ensure that all of them are permissioned to use ADL.
        for (auto accountId : m_host_if->m_account_ids)
        {
            TTLOG<<"[algo:"<< inst_id().to_string() <<"]: will use account_id="<< accountId <<"lu";
        }
        if(!m_host_if->InitPaused() && env::instance().check_adl_permission() && !m_algo_inst_mgr->CheckADLPermission(m_host_if->m_account_ids, error))
        {
            throw algojob__request_failure_adl_permission_denied;
        }

        // Earlier in CreateBlockGraph, we must have parsed toplevel algo params to determine
        // whether or not the given algo is an SOA. If so, validate the SOA params.
        if (m_host_if->IsSOA() && !m_host_if->VerifySOAParams())
        {
            throw algojob__request_failure_invalid_soa_params;
        }

        // We are ready to set the algo into motion - init the action.
        // Note that AlgoServer must have ack'd the request for this action earlier in time.
        // So we must NOT publish another ack.
        if (reason == instr_compile_new)
        {
            init_action(m_host_if->InitPaused() ? algoActionPause : algoActionStart);
        }
        else // reason == instr_compile_recovered
        {
            init_action(algoActionRecover);
        }

        try_collect_and_send_values(false);
    }
    catch (enum algojob__request_failure_code code)
    {
        process_compilation_failed_event(code, error);
    }
    catch (const std::runtime_error &e)
    {
        error = std::string(e.what());
        TTLOG<<<< error <<;
        process_compilation_failed_event(algojob__request_failure_init_failure, error);
    }
}

void
adl_algo_bin::fail_algo_on_queue_limit_violation ()
{
    m_failed_on_queue_limit_violation = true;
    process_action_failed_event();
}

void adl_algo_bin::process_action_failed_event ()
{
    // Get current synth status.
    enum ttsdk_synth_status current_synth_status = ttsdk_synth_status_working;
    TTSDK_GetSynthStatus(algo_params(), &current_synth_status);

    // Set next synth status.
    enum ttsdk_synth_status next_synth_status = ttsdk_synth_status_failed;
    TTSDK_SetSynthStatus(algo_params(), ttsdk_synth_status_failed);

    // Get current algo_state.
    enum algojob__algo_state current_algo_state = m_state.load(std::memory_order_relaxed);

    // Set next algo_state.
    enum algojob__algo_state next_algo_state = algojob__algo_failed;
    m_state.store(next_algo_state, std::memory_order_relaxed);

    // If the failed action is an update, set restatement reason to "replace_rejected".
    auto failed_action = m_pending_request.request_id;
    if (failed_action == algojob__request_update)
    {
        TTSDK_SetExecRestatementReason(algo_params(), ttsdk_exec_restatement_reason_replace_rejected);
    }

    // NOTE: The current_algo_state and next_algo_state will both be "algojob__algo_failed."
    //       This is because NotifyActionFailed sets this state before queuing a call to this function.
    TTLOG<<"[algo:"<< inst_id().to_string() <<"] process_action_failed_event: ";

    // Publish to server.
    send_status();

    // Clear the exec_restatement_reason after sending response back to algoserver_exec.
    TTSDK_ClearExecRestatementReason(algo_params());
    // Clear the trade_date after sending response back to algoserver_exec.
    TTSDKInt_ClearTradeDate(algo_params());

    // Purge child orders.
    // We make an exception for a failed recovery and leave the child orders alone,
    // to allow the failed recovery to be re-attempted.
    if (current_synth_status == ttsdk_synth_status_recovering)
    {
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] process_action_failed_event: Skipping purge child orders to allow recovery to be retried.";
        return;
    }

    // Purge child orders since the algo has faild.
    // Note that, even though we call "OnAlgoCanceled," the algo has NOT been canceled yet.
    // We are just re-using that function to purge child orders.
    if (m_algo_inst_mgr && !m_algo_inst_mgr->m_purged_child_on_cancel)
    {
        m_algo_inst_mgr->OnAlgoCanceled(nullptr);
    }
}

void adl_algo_bin::process_action_complete_event ()
{
    ScopeLogger<> scope_process_action_complete_event("process_action_complete_event");
    auto action = m_host_if->PopPendingAction();

    auto state = m_state.load(std::memory_order_relaxed);
    TTLOG<<"[algo:"<< inst_id().to_string() <<"] process_action_complete_event:action="<< ALGO_ACTION_STR[action] <<" state="<< ALGO_STATE_STR[state] <<",";

    // If the completed action is an update, set restatement reason to "replaced".
    if (action == algoif::algoActionUpdate)
    {
        TTSDK_SetExecRestatementReason(algo_params(), ttsdk_exec_restatement_reason_replaced);
    }

    // DEB-125170
    // If the completed action is start, set restatement reason to "order_accepted".
    if (action == algoif::algoActionStart)
    {
        TTSDK_SetExecRestatementReason(algo_params(), ttsdk_exec_restatement_reason_order_accepted);
    }

    tt::algoutil::AlgoEventType event_type = tt::algoutil::AlgoEventType::unknown;
    switch(state)
    {
        case algojob__algo_recovering:
            m_host_if->MarkRecoveryFileAsStale();
        case algojob__algo_starting:
            clear_pending_data();
            event_type = tt::algoutil::AlgoEventType::request_start;
        case algojob__algo_resuming:
            m_algo_inst_mgr->SetChildRiskRejected(false);
            //  Check if the event_type hasn't been set yet (this check is needed because of the fall-through logic)
            if (event_type == tt::algoutil::AlgoEventType::unknown)
            {
                event_type = tt::algoutil::AlgoEventType::request_resume;
            }
        case algojob__algo_updating_started:
        {
            //  Check if the event_type hasn't been set yet (this check is needed because of the fall-through logic)
            if (event_type == tt::algoutil::AlgoEventType::unknown)
            {
                event_type = tt::algoutil::AlgoEventType::request_other;
            }
            auto action = pending_state_to_action(state);
            state = algojob__algo_started;
            m_state.store(state, std::memory_order_relaxed);
            TTSDK_SetSynthStatus(algo_params(), ttsdk_synth_status_working);
            m_host_if->NotifyBlockActionComplete(action);
            m_host_if->ProcessActionBlocks(tt::algoutil::AlgoEventInfo(event_type));
            if (m_host_if->IsSOA())
            {
                m_host_if->OnSOAUpdated();
            }
            break;
        }

        case algojob__algo_pausing:
        {
            // AlgoInstMgr is in a library. It returns these events via 'manager::AlgoJob_ScheduleADLHostEvent( void *event) (to avoid an EventCallback)
            state = algojob__algo_detaching_for_paused;
            m_state.store(state, std::memory_order_relaxed);
            m_host_if->NotifyBlockActionComplete(algoActionPause);
            m_algo_inst_mgr->OnAlgoPaused([&] (bool result)
                                          {
                                              if (m_algo_bin.active())
                                              {
                                                  m_algo_bin.dispatch(
                                                      std::make_shared<host_event>(result
                                                                                   ? host_event_id::action_complete
                                                                                   : host_event_id::action_failed),
                                                      [&] (const std::shared_ptr<void>& data)
                                                      {
                                                          if (m_algo_bin.active())
                                                          {
                                                              m_algo_bin.m_impl->process_host_event(data);
                                                          }
                                                      },
                                                      AlgoEventType::continue_pause
                                                  );
                                              }
                                          });
            m_host_if->InitiateNextAction();
            return;
        }
        case algojob__algo_stopping:
        {
            // AlgoInstMgr is in a library. It returns these events via 'manager::AlgoJob_ScheduleADLHostEvent( void *event) (to avoid an EventCallback)
            state = algojob__algo_detaching_for_stopped;
            m_state.store(state, std::memory_order_relaxed);
            m_host_if->NotifyBlockActionComplete(algoActionStop);
            m_algo_inst_mgr->OnAlgoCanceled([&] (bool result)
                                            {
                                                if (m_algo_bin.active())
                                                {
                                                    m_algo_bin.dispatch(
                                                        std::make_shared<host_event>(result
                                                                                     ? host_event_id::action_complete
                                                                                     : host_event_id::action_failed),
                                                        [&] (const std::shared_ptr<void>& data)
                                                        {
                                                            if (m_algo_bin.active())
                                                            {
                                                                m_algo_bin.m_impl->process_host_event(data);
                                                            }
                                                        },
                                                        AlgoEventType::continue_stop
                                                    );
                                                }
                                            });
            m_host_if->InitiateNextAction();
            return;
        }

        case algojob__algo_detaching_for_paused:
            state = algojob__algo_paused;
            m_state.store(state, std::memory_order_relaxed);
            TTSDK_SetSynthStatus(algo_params(), ttsdk_synth_status_paused);
            break;

        case algojob__algo_detaching_for_stopped:
            state = algojob__algo_stopped;
            m_state.store(state, std::memory_order_relaxed);
            TTSDK_SetSynthStatus(algo_params(), ttsdk_synth_status_canceled);
            break;

        case algojob__algo_updating_paused:
            state = algojob__algo_paused;
            m_state.store(state, std::memory_order_relaxed);
            m_host_if->NotifyBlockActionComplete(algoActionUpdate);
            m_host_if->ProcessActionBlocks(tt::algoutil::AlgoEventInfo(tt::algoutil::AlgoEventType::request_pause_or_cancel));
            if (m_host_if->IsSOA())
            {
                m_host_if->OnSOAUpdated();
            }
            break;

        default:
            return;
    }

    if(m_pending_request.pending)
    {
        server_connection::instance().send_request_complete(
            m_pending_request,
            state,
            inst_id(),
            &m_algo_params
        );
        clear_pending_data();
    }
    else
    {
        send_status();
    }

    // Clear the exec_restatement_reason after sending response back to algoserver_exec.
    TTSDK_ClearExecRestatementReason(algo_params());
    // Clear the trade_date after sending response back to algoserver_exec.
    TTSDKInt_ClearTradeDate(algo_params());

    if(state == algojob__algo_stopped)
    {
        schedule_destroy();
    }
    else
    {
        m_host_if->InitiateNextAction();
    }
}

void adl_algo_bin::run_periodic_host_task ()
{
    if (m_inst_if)
    {
        try_collect_and_send_values(false);
    }

    if (m_host_if)
    {
        m_host_if->FlushAlert();
    }
}

void adl_algo_bin::reject_request (const std::shared_ptr<algojob__request>& request)
{
    auto state = m_state.load(std::memory_order_relaxed);
    ALGO_WLOG("[algo:%s] request[%s][id:%llu] rejected. state=%s",
              inst_id().to_string(),
              ALGOJOB_REQUEST_ID_STR[request->id],
              request->user_request_id,
              ALGO_STATE_STR[state]);

    server_connection::instance().send_request_failure(
        request->id,
        request->flags,
        request->user_request_id,
        state,
        algojob__request_failure_algo_busy,
        inst_id(),
        &m_algo_params
    );
}

void adl_algo_bin::suppress_action ()
{
    auto state = m_state.load(std::memory_order_relaxed);
    if (state == algojob__algo_started)
    {
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] suppress action";
        if (m_host_if)
        {
            m_host_if->SetSuppressActionFlag(algoif::algoBoolTrue);
        }
    }
    else
    {
        ALGO_WLOG("[algo:%s] skip SetSuppressActionFlag. state=%s", inst_id().to_string(), ALGO_STATE_STR[state]);
    }
}

void adl_algo_bin::save_state_to_disk ()
{
    if (m_host_if)
    {
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] Saving state to disk.";
        m_host_if->SaveStateToDisk();
    }
}

void adl_algo_bin::recover_children ()
{
    TTLOG<<"[algo:"<< inst_id().to_string() <<"] recover_children: Downloading children.";

    // The "delete_pr" flag is relevant in the case where this algo instance submitted
    // a PR child order during the prior session, but is being recovered onto a
    // version / server that cannot support PR. In such case, "delete_pr" flag will
    // be set, and OrderD will attempt to cancel the PR child order if Bookie returns any.
    // The rest of recovery state machine will proceed as if the algo instance had never
    // submitted a PR child order to begin with.
    bool delete_pr = false;
    if (!env::instance().is_sse_pr_enabled())
    {
        delete_pr = true;
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] recover_children: Will attempt to delete PR child order (if any) because: ";
    }

    // OrderD will download the latest snapshots for all child orders,
    // including the PR order (if any). For each downloaded child order,
    // an order_handle will be created in OrderD.
    TTSDKInt_RecoverChildOrder(&inst_id().value,
                               &OnChildrenRecoveryResponse,
                               reinterpret_cast<ttsdk_algo_inst_mgr>(m_algo_inst_mgr.get()),
                               this,
                               ttsdk_algo_type_adl,
                               delete_pr,
                               ttsdk_order_download_reason_recovery);
}

/* static */ void
adl_algo_bin::OnChildrenRecoveryResponse (bool result, ttsdk_params* params, size_t size, void* user_data)
{
    if (user_data == nullptr)
    {
        // This is an unexpected scenario - bail out to prevent crash.
        // Logger is inaccessible in this STATIC function.
        return;
    }

    auto adl_algo_bin = static_cast<algojob::adl_algo_bin*>(user_data);
    adl_algo_bin->on_children_recovered(result, params, size);
}

void adl_algo_bin::on_children_recovered (bool result, ttsdk_params* params, size_t size)
{
    auto state = m_state.load(std::memory_order_relaxed);
    TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: ";

    if (state == algojob__algo_stopping)
    {
        // Algo was stopped while downloading children.

        // Send back the Cancel OK.
        server_connection::instance().send_request_complete(
            m_pending_request,
            algojob__algo_stopped,
            inst_id(),
            &m_algo_params
        );

        // It is possible there was a price subscription made and a price update has been queued already.
        // So queue the destruction to flush the queued price updates instead of destroying immediately.
        schedule_destroy();
        return;
    }

    if (!result)
    {
        TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: Download failed, declaring recovery failure!";
        process_compilation_failed_event(algojob__request_failure_to_recover_child_orders, "Failed to recover children");
        return;
    }

    TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: Download success, printing recovered child order IDs.";
    for (size_t i = 0; i < size; i++)
    {
        const ttsdk_params child_param = params[i];
        if (child_param == nullptr)
        {
            TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: Param for a recovered child was unexpectedly a nullptr: ";
            continue;
        }

        tt::algoutil::ttuuid order_id(false);
        if (TTSDK_GetOrderId(child_param, &order_id.value) != ttsdk_error_none)
        {
            TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: Order ID missing for a recovered child: ";
            continue;
        }

        TTLOG<<"[algo:"<< inst_id().to_string() <<"] on_children_recovered: ";
    }

    create_block_graph_and_init_action(instr_compile_recovered);
}

/* static */ std::string
adl_algo_bin::instr_compile_reason_to_string (enum instr_compile_reason reason)
{
    std::string to_return = "unknown";
    switch (reason)
    {
        case instr_compile_new:
            to_return = "new";
            break;
        case instr_compile_recovered:
            to_return = "recovered";
            break;
        default:
            break;
    }
    return to_return;
}