#include <string>
#include <iostream>
#include <sstream>

#include <inttypes.h>

#include "nnfdm/client.h"

extern "C" {
#include "nnfdm.h"
#include "axl_internal.h"
#include "kvtree.h"
}

namespace nnfdm = near_node_flash::data_movement;

namespace {
    nnfdm::DataMoverClient* nnfdm_client{nullptr};
    nnfdm::Workflow* nnfdm_workflow{nullptr};

    std::string print_status(nnfdm::StatusResponse& status_response)
    {
        std::stringstream ss;

        ss << "Offload Command Status: " << "{" << status_response.state() << "/";
        switch (status_response.state()) {
            case nnfdm::StatusResponse::State::STATE_PENDING:    ss << "STATE_PENDING";   break;
            case nnfdm::StatusResponse::State::STATE_STARTING:   ss << "STATE_STARTING";  break;
            case nnfdm::StatusResponse::State::STATE_RUNNING:    ss << "STATE_RUNNING";   break;
            case nnfdm::StatusResponse::State::STATE_COMPLETED:  ss <<"STATE_COMPLETED";  break;
            case nnfdm::StatusResponse::State::STATE_CANCELLING: ss <<"STATE_CANCELLING"; break;
            case nnfdm::StatusResponse::State::STATE_UNKNOWN:    ss << "STATE_UNKNOWN";   break;
            default:                                             ss << "STATE_???";       break;
        }
        ss << "}, ";

        ss << "{" << status_response.status() << "/";
        switch (status_response.status()) {
            case nnfdm::StatusResponse::Status::STATUS_INVALID:   ss << "STATUS_INVALID";   break;
            case nnfdm::StatusResponse::Status::STATUS_NOT_FOUND: ss << "STATUS_NOT_FOUND"; break;
            case nnfdm::StatusResponse::Status::STATUS_SUCCESS:   ss << "STATUS_SUCCESS";   break;
            case nnfdm::StatusResponse::Status::STATUS_FAILED:    ss << "STATUS_FAILED";    break;
            case nnfdm::StatusResponse::Status::STATUS_CANCELLED: ss << "STATUS_CANCELLED"; break;
            case nnfdm::StatusResponse::Status::STATUS_UNKNOWN:   ss << "STATUS_UNKNOWN";   break;
            default:                                              ss << "STATE_???";        break;
        }
        ss << "}" << std::endl;

        nnfdm::CommandStatus cmd = status_response.commandStatus();
        ss << "    Offload Command Status:" << std::endl;
        ss << "      Command: " << cmd.command <<  std::endl;
        ss << "      Progress: " << cmd.progress <<  "%" << std::endl;
        ss << "      ElapsedTime: " << cmd.elapsedTime <<  std::endl;
        ss << "      LastMessage: " << cmd.lastMessage <<  std::endl;
        ss << "      LastMessageTime: " << cmd.lastMessageTime <<  std::endl;
        ss << "    Offload StartTime: " << status_response.startTime() << std::endl;
        ss << "    Offload EndTime: " << status_response.endTime() << std::endl;
        ss << "    Offload Message: " << status_response.message() << std::endl;

        return ss.str();
    }

    int nnfdm_stat(const char* fname, const char* uid, int64_t max_seconds_to_wait)
    {
        AXL_DBG(1, "%s", fname);
        int rval = 0;
        int retry_count = 0;
        nnfdm::StatusResponse status_response;
        nnfdm::RPCStatus rpc_status{ nnfdm_client->Status(*nnfdm_workflow,  nnfdm::StatusRequest{std::string{uid}, max_seconds_to_wait}, &status_response) };

        if (!rpc_status.ok()) {
            AXL_ABORT(    -1
                        , "NNFDM Status RPC FAILED %d: %s @ %s:%d"
                        , rpc_status.error_code()
                        , rpc_status.error_message().c_str()
                        , __FILE__
                        , __LINE__);
            /*NOTREACHED*/
        }

        switch (status_response.state()) {
            case nnfdm::StatusResponse::State::STATE_PENDING:
            case nnfdm::StatusResponse::State::STATE_STARTING:
            case nnfdm::StatusResponse::State::STATE_RUNNING:
                rval = AXL_STATUS_INPROG;
                break;
            case nnfdm::StatusResponse::State::STATE_COMPLETED:
                if (status_response.status() == nnfdm::StatusResponse::Status::STATUS_SUCCESS) {
                    AXL_DBG(1, "Offload Complete(%s)", fname );
                    rval = AXL_STATUS_DEST;
                }
                else {
                    rval = AXL_STATUS_ERROR;
                    AXL_ERR(  "NNFDM Offload Status UNSUCCESSFUL: %d\n%s"
                            , status_response.status()
                            , print_status(status_response).c_str());
                }
                break;
            default:
                AXL_ABORT(   -1
                           , "NNFDM Offload State STATE UNKNOWN: %d\n%s"
                           , status_response.status()
                           , print_status(status_response).c_str());
                break;
        }

        return rval;
    }
}   // End of empty namepace

extern "C" {

void nnfdm_init()
{
    AXL_DBG(1, "%p", nnfdm_client);
    if (nnfdm_client == nullptr) {
        const std::string socket_name{"unix:///var/run/nnf-dm.sock"};
        const std::string workflow_name{getenv("DW_WORKFLOW_NAME")};
        const std::string workflow_namespace{getenv("DW_WORKFLOW_NAMESPACE")};

        nnfdm_client = new nnfdm::DataMoverClient{ socket_name };
        if (nnfdm_client == nullptr) {
            AXL_ABORT(-1
                      , "NNFDM init: Failed to create data movement client "
                        "instance for %s@ %s:%d"
                      , socket_name.c_str()
                      , __FILE__
                      , __LINE__);
        }

        nnfdm_workflow = new nnfdm::Workflow{workflow_name, workflow_namespace};
        if (nnfdm_workflow == nullptr) {
            AXL_ABORT(  -1
                      , "nnfdm_init: Failed to create data movement workflow "
                        "instance for %s/%s@ %s:%d"
                      , workflow_name.c_str()
                      , workflow_namespace.c_str()
                      , __FILE__
                      , __LINE__);
        }
    }
}

void nnfdm_finalize()
{
    AXL_DBG(1, "%p", nnfdm_client);
    if (nnfdm_client) {
        delete nnfdm_workflow;
        delete nnfdm_client;

        nnfdm_workflow = nullptr;
        nnfdm_client = nullptr;
    }
}

int nnfdm_start(int id)
{
    AXL_DBG(1, "%d", id);
    int rc = AXL_SUCCESS;

    /* Record that we started transfer of this file list */
    kvtree* file_list = axl_kvtrees[id];
    kvtree_util_set_int(file_list, AXL_KEY_STATUS, AXL_STATUS_INPROG);
 
    kvtree_elem* elem;
    kvtree* files = kvtree_get(file_list, AXL_KEY_FILES);
    for (  kvtree_elem* elem = kvtree_elem_first(files);
           elem != NULL;
           elem = kvtree_elem_next(elem))
    {
        char* src_filename = kvtree_elem_key(elem);
        kvtree* elem_hash = kvtree_elem_hash(elem);
 
        /* get the destination for this file */
        char* dst_filename;
        kvtree_util_get_str(elem_hash, AXL_KEY_FILE_DEST, &dst_filename);

        AXL_DBG(1, "nnfdm::CreateRequest(src=%s, dst=%s)" , src_filename, dst_filename);
        nnfdm::CreateRequest create_request(  
              std::string{src_filename} // Source file or directory
            , std::string{dst_filename} // Destination file or directory 
            , false                     // If True, the data movement command runs `/bin/true` rather than perform actual data movement
            , ""                        // Extra options to pass to `dcp` if present in the Data Movement command.
            , false                     // If true, enable server-side logging of stdout when the command is successful. Failures output is always logged.
            , true                      // If true, store stdout in DataMovementStatusResponse.Message when the command is successful. Failure output is always contained in the message.
            , -1                        // The number of slots specified in the MPI hostfile. A value of 0 disables the use of slots in the hostfile. -1 will defer to the server side configuration.
            , -1                        // The number of max_slots specified in the MPI hostfile. A value of 0 disables the use of max_slots in the hostfile. -1 will defer to the server side configuration.
            , "scr"                     // Data movement profile.  Empty will default to the default profile.
        );
        
        nnfdm::CreateResponse create_response;
        nnfdm::RPCStatus rpc_status = nnfdm_client->Create( *nnfdm_workflow, create_request, &create_response);
        if (!rpc_status.ok()) {
            AXL_ERR("%s: NNFDM Create(%s, %s) RPC failed with error %d (%s)"
                      , __PRETTY_FUNCTION__, src_filename, dst_filename
                      , rpc_status.error_code(), rpc_status.error_message().c_str());
            kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, AXL_STATUS_ERROR);
            rc = AXL_FAILURE;
        }

        if (create_response.status() == nnfdm::CreateResponse::Status::STATUS_SUCCESS) {
            /* record that the file is in progress */
            kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, AXL_STATUS_INPROG);
            kvtree_util_set_str(elem_hash, AXL_KEY_FILE_SESSION_UID, create_response.uid().c_str());
        }
        else {
            AXL_ERR("%s: NNFDM Create(%s, %s) Response Status is not SUCCESS: response status: %d - %s"
                    , __PRETTY_FUNCTION__, src_filename, dst_filename, create_response.status(), create_response.message().c_str() );
            kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, AXL_STATUS_ERROR);
            rc = AXL_FAILURE;
        } 
    }

    return rc;
}

int nnfdm_test(int id)
{
    AXL_DBG(1, "%d", id);
    kvtree* file_list = axl_kvtrees[id];
    kvtree* files     = kvtree_get(file_list, AXL_KEY_FILES);

    /* iterate/wait over in-progress files */
    for (kvtree_elem* elem = kvtree_elem_first(files); elem != NULL; elem = kvtree_elem_next(elem)) {
        char* src_filename = kvtree_elem_key(elem);
        int status;
        char* uid;

        kvtree* elem_hash = kvtree_elem_hash(elem);
        kvtree_util_get_int(elem_hash, AXL_KEY_FILE_STATUS, &status);

        if (status == AXL_STATUS_DEST) {
            continue;   /* This one is done */
        }

        kvtree_util_get_str(elem_hash, AXL_KEY_FILE_SESSION_UID, &uid);
        status = nnfdm_stat(src_filename, uid, 1);

        if (status != AXL_STATUS_DEST) {
            return AXL_FAILURE;   /* At least one file is not done */
        }

        kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, status);
    }

    return AXL_SUCCESS;
}

int nnfdm_cancel(int id)
{
    AXL_DBG(1, "%d", id);
    kvtree* file_list = axl_kvtrees[id];
    kvtree* files     = kvtree_get(file_list, AXL_KEY_FILES);

    /* iterate/wait over in-progress files */
    for (  kvtree_elem* elem = kvtree_elem_first(files);
           elem != NULL;
           elem = kvtree_elem_next(elem))
    {
        int status;
        char* uid;
        char* src_filename = kvtree_elem_key(elem);
        kvtree* elem_hash = kvtree_elem_hash(elem);
        kvtree_util_get_int(elem_hash, AXL_KEY_FILE_STATUS, &status);

        if (status == AXL_STATUS_DEST) {
            continue;   /* This one is done */
        }

        kvtree_util_get_str(elem_hash, AXL_KEY_FILE_SESSION_UID, &uid);

        nnfdm::CancelRequest cancel_request(uid);
        nnfdm::CancelResponse cancel_response;
        nnfdm::RPCStatus rpc_status = nnfdm_client->Cancel(  *nnfdm_workflow
                                                           ,  cancel_request
                                                           , &cancel_response);
        if (!rpc_status.ok()) {
            AXL_ABORT(-1,
                "NNFDM Cancel(uid=%s, file=%s) failed with error %d (%s) @ %s:%d"
                , uid
                , src_filename
                , rpc_status.error_code()
                , rpc_status.error_message().c_str()
                , __FILE__
                , __LINE__
            );
            /*NOTREACHED*/
        }

        switch (cancel_response.status()) {
            case nnfdm::CancelResponse::STATUS_NOT_FOUND:
                //
                // Assume that unfound uids simply completed previously
                //
                AXL_DBG(1, "NNFDM Cancel(uid=%s, file=%s) NOTFOUND - IGNORING", uid, src_filename);
                continue;
            case nnfdm::CancelResponse::STATUS_SUCCESS:
                AXL_DBG(1, "NNFDM Cancel(uid=%s, file=%s) Canceled @ %s:%d", uid, src_filename);
                break;
            default:
                AXL_ABORT(    -1
                            , "NNFDM Cancel(uid=%s, file=%s) "
                              "Failed with error %d - IGNORING @ %s:%d"
                            , uid
                            , src_filename
                            , cancel_response.status()
                            , __FILE__
                            , __LINE__);
                /*NOTEACHED*/
                return 1;
        }

        // Now delete the associated uid
        nnfdm::DeleteRequest delete_request(std::string{uid});
        nnfdm::DeleteResponse deleteResponse;
        rpc_status = nnfdm_client->Delete(   *nnfdm_workflow
                                           ,  delete_request
                                           , &deleteResponse);

        if (!rpc_status.ok()) {
            AXL_ABORT(   -1
                       , "NNFDM Delete(uid=%s, file=%s) RPC FAILED: "
                         "%d (%s) @ %s:%d"
                       , uid
                       , src_filename
                       , rpc_status.error_code()
                       , rpc_status.error_message().c_str()
                       , __FILE__
                       , __LINE__);
            /*NOTEACHED*/
        }

        /* Delete the request */
        switch (deleteResponse.status()) {
            case nnfdm::DeleteResponse::STATUS_SUCCESS:
                break;
            default:
                AXL_ABORT(-1,
                    "NNFDM Offload Delete(%s) UNSUCCESSFUL: %d (%s) @ %s:%d",
                    src_filename, deleteResponse.status(), deleteResponse.message().c_str(),
                    __FILE__, __LINE__);
                return 1;
        }

        kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, AXL_STATUS_DEST);
    }

    return AXL_SUCCESS;
}

int nnfdm_wait(int id)
{
    AXL_DBG(1, "id=%d", id);
    const int64_t max_seconds_to_wait{1}; // wait 1 second
    kvtree* file_list = axl_kvtrees[id];
    kvtree* files     = kvtree_get(file_list, AXL_KEY_FILES);

    /* iterate/wait over in-progress files */
    for (kvtree_elem* elem = kvtree_elem_first(files); elem != NULL; elem = kvtree_elem_next(elem)) {
        char* src_filename = kvtree_elem_key(elem);
        kvtree* elem_hash = kvtree_elem_hash(elem);

        char* uid;
        kvtree_util_get_str(elem_hash, AXL_KEY_FILE_SESSION_UID, &uid);

        int status;
        do {
            status = nnfdm_stat(src_filename, uid, max_seconds_to_wait);
        } while (status == AXL_STATUS_INPROG);

        nnfdm::DeleteRequest delete_request(std::string{uid});
        nnfdm::DeleteResponse deleteResponse;

        nnfdm::RPCStatus rpc_status = nnfdm_client->Delete(*nnfdm_workflow, delete_request, &deleteResponse);
        if (!rpc_status.ok()) {
            AXL_ABORT(-1, "NNFDM Delete RPC FAILED: %d (%s)", rpc_status.error_code(), rpc_status.error_message().c_str());
            /*NOTEACHED*/
        }

        /* Delete the request */
        switch (deleteResponse.status()) {
            case nnfdm::DeleteResponse::STATUS_SUCCESS:
                break;
            default:
                AXL_ABORT(-1,
                    "NNFDM Offload Delete(%s) UNSUCCESSFUL: %d (%s)",
                    src_filename, deleteResponse.status(), deleteResponse.message().c_str());
                return 1;
        }

        AXL_DBG(1, "%s Marked Done", src_filename);
        kvtree_util_set_int(elem_hash, AXL_KEY_FILE_STATUS, status);
    }

    /* iterate over files */
    for (kvtree_elem* elem = kvtree_elem_first(files); elem != NULL; elem = kvtree_elem_next(elem)) {
        char* src_filename = kvtree_elem_key(elem);
        kvtree* elem_hash = kvtree_elem_hash(elem);

        int status;
        kvtree_util_get_int(elem_hash, AXL_KEY_FILE_STATUS, &status);

        switch (status) {
            case AXL_STATUS_DEST:
                AXL_DBG(1, "%s Is marked as done", src_filename);
                break;
            case AXL_STATUS_SOURCE:
            case AXL_STATUS_ERROR:
            default:
                AXL_ABORT(-1,
                    "Wait operation called on file with invalid status @ %s:%d",
                    __FILE__, __LINE__
                );
        }
    }

    /* record transfer complete */
    AXL_DBG(1, "File List is done");
    kvtree_util_set_int(file_list, AXL_KEY_STATUS, AXL_STATUS_DEST);
    return AXL_SUCCESS;
}
}
