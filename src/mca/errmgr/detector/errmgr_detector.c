/*
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/util/output.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/base/prte_mca_base_var.h"
#include "src/threads/threads.h"
#include "src/mca/rml/rml.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/bmg/grpcomm_bmg.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/data_type_support/prte_dt_support.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

#include "src/mca/propagate/propagate.h"
#include "errmgr_detector.h"
#include <math.h>

static int init(void);
static int finalize(void);

int prte_errmgr_enable_detector(bool flag);
/******************
 * detector module
 ******************/
prte_errmgr_base_module_t prte_errmgr_detector_module = {
    init,
    finalize,
    prte_errmgr_base_log,
    prte_errmgr_base_abort,
    prte_errmgr_base_abort_peers,
    prte_errmgr_enable_detector
};

prte_errmgr_base_module_t prte_errmgr = {
    init,
    finalize,
    prte_errmgr_base_log,
    NULL,
    NULL,
    prte_errmgr_enable_detector
};

/*
 * Local functions
 */
static int fd_heartbeat_request(prte_errmgr_detector_t* detector);
static int fd_heartbeat_send(prte_errmgr_detector_t* detector);

static int fd_heartbeat_request_cb(int status,
        prte_process_name_t* sender,
        prte_buffer_t *buffer,
        prte_rml_tag_t tg,
        void *cbdata);

static int fd_heartbeat_recv_cb(int status,
        prte_process_name_t* sender,
        prte_buffer_t *buffer,
        prte_rml_tag_t tg,
        void *cbdata);

static double Wtime();
//static double prte_errmgr_heartbeat_period = 5;
//static double prte_errmgr_heartbeat_timeout = 10;
static prte_event_base_t* fd_event_base = NULL;

static void fd_event_cb(int fd, short flags, void* pdetector);


static int pack_state_for_proc(prte_buffer_t *alert, prte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &(child->name.vpid), 1, PRTE_VPID))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &child->pid, 1, PRTE_PID))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &child->state, 1, PRTE_PROC_STATE))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &child->exit_code, 1, PRTE_EXIT_CODE))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}

static void register_cbfunc(int status, size_t errhndler, void *cbdata)
{
    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                "errmgr:detector:event register cbfunc with status %d ", status));
}

static void error_notify_cbfunc(size_t evhdlr_registration_id,
        pmix_status_t status,
        const pmix_proc_t *psource,
        pmix_info_t info[], size_t ninfo,
        pmix_info_t *results, size_t nresults,
        pmix_event_notification_cbfunc_fn_t cbfunc,
        void *cbdata)
{
    prte_process_name_t proc, source;
    proc.jobid = PRTE_JOBID_INVALID;
    proc.vpid = PRTE_VPID_INVALID;

    int rc;
    prte_proc_t *temp_prte_proc;
    prte_buffer_t *alert;
    prte_job_t *jdata;
    prte_plm_cmd_flag_t cmd;
    size_t n;
    PRTE_PMIX_CONVERT_PROCT(rc, &source, psource);
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PRTE_PMIX_CONVERT_PROCT(rc, &proc, info[n].value.data.proc);

                if( prte_get_proc_daemon_vpid(&proc) != PRTE_PROC_MY_NAME->vpid){
                    return;
                }
                PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                            "%s errmgr: detector: error proc %s with key-value %s notified from %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc),
                            info[n].key, PRTE_NAME_PRINT(&source)));

                if (NULL == (jdata = prte_get_job_data_object(proc.jobid))) {
                    /* must already be complete */
                    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                "%s errmgr:detector:error_notify_callback NULL jdata - ignoring error",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                }
                temp_prte_proc= (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, proc.vpid);

                alert = PRTE_NEW(prte_buffer_t);
                /* pack update state command */
                cmd = PRTE_PLM_UPDATE_PROC_STATE;
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &cmd, 1, PRTE_PLM_CMD))) {
                    PRTE_ERROR_LOG(rc);
                    return;
                }

                /* pack jobid */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &proc.jobid, 1, PRTE_JOBID))) {
                    PRTE_ERROR_LOG(rc);
                    return;
                }

                /* proc state now is PRTE_PROC_STATE_ABORTED_BY_SIG, cause odls set state to this; code is 128+9 */
                temp_prte_proc->state = PRTE_PROC_STATE_ABORTED_BY_SIG;
                /* now pack the child's info */
                if (PRTE_SUCCESS != (rc = pack_state_for_proc(alert, temp_prte_proc))) {
                    PRTE_ERROR_LOG(rc);
                    return;
                }

                /* send this process's info to hnp */
                if (0 > (rc = prte_rml.send_buffer_nb(
                                PRTE_PROC_MY_HNP, alert,
                                PRTE_RML_TAG_PLM,
                                prte_rml_send_callback, NULL))) {
                    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                "%s errmgr:detector: send to hnp failed",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(alert);
                }
                if (PRTE_FLAG_TEST(temp_prte_proc, PRTE_PROC_FLAG_IOF_COMPLETE) &&
                        PRTE_FLAG_TEST(temp_prte_proc, PRTE_PROC_FLAG_WAITPID) &&
                        !PRTE_FLAG_TEST(temp_prte_proc, PRTE_PROC_FLAG_RECORDED)) {
                    PRTE_ACTIVATE_PROC_STATE(&proc, PRTE_PROC_STATE_TERMINATED);
                }

                prte_propagate.prp(&source.jobid, &source, &proc, PRTE_ERR_PROC_ABORTED);
                break;
            }
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PRTE_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static int init(void) {
    fd_event_base = prte_sync_event_base;

    if ( PRTE_PROC_IS_DAEMON )
    {
        prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_HEARTBEAT_REQUEST,
                PRTE_RML_PERSISTENT,fd_heartbeat_request_cb,NULL);
        prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_HEARTBEAT,
                PRTE_RML_PERSISTENT,fd_heartbeat_recv_cb,NULL);
    }
    return PRTE_SUCCESS;
}

int finalize(void) {
    if ( PRTE_PROC_IS_DAEMON )
    {
        prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;

        if(detector->hb_observer != PRTE_VPID_INVALID)
        {
            detector->hb_observer = prte_process_info.my_name.vpid;
            PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,"errmgr:detector: send last heartbeat message"));
            fd_heartbeat_send(detector);
            detector->hb_period = INFINITY;
        }
        prte_event_del(&prte_errmgr_world_detector.fd_event);
        prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_HEARTBEAT_REQUEST);
        prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_HEARTBEAT);
        if( prte_sync_event_base != fd_event_base ) prte_event_base_free(fd_event_base);

        /* set heartbeat peroid to infinity and observer to invalid */
        prte_errmgr_world_detector.hb_period = INFINITY;
        prte_errmgr_world_detector.hb_observer = PRTE_VPID_INVALID;
    }
    return PRTE_SUCCESS;
}

bool errmgr_get_daemon_status(prte_process_name_t daemon)
{
    prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;
    int i;
    for ( i=0; i<detector->failed_node_count; i++)
    {
        if( *(detector->daemons_state +i) == daemon.vpid)
        {
            return false;
        }
    }
    return true;
}

void errmgr_set_daemon_status(prte_process_name_t daemon)
{
    prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;
    *(detector->daemons_state + detector->failed_node_count) = daemon.vpid;
}

static double Wtime(void)
{
    double wtime;

#if PRTE_TIMER_CYCLE_NATIVE
    wtime = ((double) prte_timer_base_get_cycles()) / prte_timer_base_get_freq();
#elif PRTE_TIMER_USEC_NATIVE
    wtime = ((double) prte_timer_base_get_usec()) / 1000000.0;
#else
    /* Fall back to gettimeofday() if we have nothing else */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    wtime = tv.tv_sec;
    wtime += (double)tv.tv_usec / 1000000.0;
#endif
    return wtime;
}

int prte_errmgr_enable_detector(bool enable_flag)
{
    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                "%s errmgr:detector report detector_enable_status %d",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), enable_flag));

    if ( PRTE_PROC_IS_DAEMON && enable_flag )
    {
        prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;
        int  ndmns, i;
        uint32_t vpid;

        pmix_status_t pcode = prte_pmix_convert_rc(PRTE_ERR_PROC_ABORTED);

        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "%s errmgr:detector: register evhandler in errmgr",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIx_Register_event_handler(&pcode, 1, NULL, 0, error_notify_cbfunc, register_cbfunc, NULL);
        prte_propagate.register_cb();

        /* num of daemon in this jobid */
        ndmns = prte_process_info.num_daemons - 1;
        vpid = prte_process_info.my_name.vpid;
        /*  we observing somebody {n,1,2,...n-1}, the ring */
        if( 0 != (vpid - 1) )
            detector->hb_observing = vpid - 1;
        else detector->hb_observing = ndmns;
        /* someone is observing us: range [1~n], the observing ring */
        detector->hb_observer = (ndmns+vpid) % ndmns + 1 ;
        detector->hb_period = prte_errmgr_heartbeat_period;
        detector->hb_timeout = prte_errmgr_heartbeat_timeout;
        detector->hb_sstamp = 0.;
        /* give some slack for MPI_Init */
        detector->hb_rstamp = Wtime()+(double)ndmns;

        detector->daemons_state = malloc(8* sizeof(int));
        for(i=0; i<8; i++)
        {
            *(detector->daemons_state + i) = -1;
        }

        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector daemon %d observering %d observer %d",
                    vpid,
                    detector->hb_observing,
                    detector->hb_observer));

        prte_event_set(fd_event_base, &detector->fd_event, -1, PRTE_EV_TIMEOUT | PRTE_EV_PERSIST, fd_event_cb, detector);
        struct timeval tv;
        tv.tv_sec = (int)(detector->hb_period / 10.);
        tv.tv_usec = (-tv.tv_sec + (detector->hb_period / 10.)) * 1e6;
        prte_event_add(&prte_errmgr_world_detector.fd_event, &tv);
    }
    return PRTE_SUCCESS;
}

static int fd_heartbeat_request(prte_errmgr_detector_t* detector) {

    int ret,  ndmns;
    uint32_t vpid;

    prte_process_name_t temp_proc_name;
    temp_proc_name.jobid = prte_process_info.my_name.jobid;
    temp_proc_name.vpid = detector->hb_observing;

    if( errmgr_get_daemon_status(temp_proc_name) )
    {
        /* already observing a live process, so nothing to do. */
        return PRTE_SUCCESS;
    }

    ndmns = prte_process_info.num_daemons-1;

    prte_buffer_t *buffer = NULL;
    prte_process_name_t daemon;
    for( vpid = (ndmns+detector->hb_observing) % ndmns;
            vpid != prte_process_info.my_name.vpid;
            vpid = (ndmns+vpid-1) % ndmns ) {
        daemon.jobid = prte_process_info.my_name.jobid;
        if(0 != vpid){
            daemon.vpid = vpid;
        }
        else daemon.vpid = ndmns;

        // this daemon is not alive
        if(!errmgr_get_daemon_status(daemon)) continue;

        /* everyone is gone, i dont need to monitor myself */
        if(daemon.vpid == prte_process_info.my_name.vpid)
        {
            detector->hb_observer = detector->hb_observing = PRTE_VPID_INVALID;
            detector->hb_rstamp = INFINITY;
            detector->hb_period = INFINITY;
            return PRTE_SUCCESS;
        }

        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector hb request updating ring"));
        detector->hb_observing = daemon.vpid;
        buffer = PRTE_NEW(prte_buffer_t);
        if (PRTE_SUCCESS != (ret = prte_dss.pack(buffer, &prte_process_info.my_name.jobid, 1,PRTE_JOBID))) {
            PRTE_ERROR_LOG(ret);
        }
        if (PRTE_SUCCESS != (ret = prte_dss.pack(buffer, &prte_process_info.my_name.vpid, 1,PRTE_VPID))) {
            PRTE_ERROR_LOG(ret);
        }
        if (0 > (ret = prte_rml.send_buffer_nb(
                        &daemon, buffer,
                        PRTE_RML_TAG_HEARTBEAT_REQUEST, prte_rml_send_callback, NULL))) {
            PRTE_ERROR_LOG(ret);
        }
        break;
    }
    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                "errmgr:detector updated ring daemon %d observering %d observer %d",
                PRTE_PROC_MY_NAME->vpid,
                detector->hb_observing,
                detector->hb_observer));
    /* we add one timeout slack to account for the send time */
    detector->hb_rstamp = Wtime()+detector->hb_timeout;
    return PRTE_SUCCESS;
}

static int fd_heartbeat_request_cb(int status, prte_process_name_t* sender,
        prte_buffer_t *buffer,
        prte_rml_tag_t tg, void *cbdata) {
    prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;
    int ndmns, rr, ro;
    prte_jobid_t vpid, jobid;
    int temp;
    temp =1;
    int rc;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &jobid,&temp,PRTE_JOBID)))
        PRTE_ERROR_LOG(rc);
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &vpid, &temp,PRTE_VPID)))
        PRTE_ERROR_LOG(rc);
    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                "errmgr:detector %d receive %d",
                prte_process_info.my_name.vpid,
                detector->hb_observer));
    ndmns = prte_process_info.num_nodes;
    rr = (ndmns-prte_process_info.my_name.vpid+vpid) % ndmns; /* translate msg->from in circular space so that myrank==0 */
    ro = (ndmns-prte_process_info.my_name.vpid+detector->hb_observer) % ndmns; /* same for the observer rank */
    if( rr < ro ) {
        return false; /* never forward on the rbcast */
    }

    detector->hb_observer = vpid;
    detector->hb_sstamp = 0.;

    fd_heartbeat_send(detector);
    return false;
}

/*
 * event loop and thread
 */

static void fd_event_cb(int fd, short flags, void* pdetector) {
    // need to find a new time func
    double stamp = Wtime();
    prte_errmgr_detector_t* detector = pdetector;

    // temp proc name for get the prte object
    prte_process_name_t temp_proc_name;

    if( (stamp - detector->hb_sstamp) >= detector->hb_period ) {
        fd_heartbeat_send(detector);
    }
    if( INFINITY == detector->hb_rstamp ) return;

    if( (stamp - detector->hb_rstamp) > detector->hb_timeout ) {
        /* this process is now suspected dead. */
        temp_proc_name.jobid = prte_process_info.my_name.jobid;
        temp_proc_name.vpid = detector->hb_observing;
        /* if first time detected */
        if (errmgr_get_daemon_status(temp_proc_name)){
            PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                        "errmgr:detector %d detected daemon %d failed, heartbeat delay",
                        prte_process_info.my_name.vpid, detector->hb_observing));
            prte_propagate.prp(&temp_proc_name.jobid, NULL, &temp_proc_name,PRTE_ERR_PROC_ABORTED );

            /* with every 8 failed nodes realloc 8 more slots to store the vpid of failed nodes */
            if( (detector->failed_node_count / 8) > 0 && (detector->failed_node_count % 8) == 0 )
                detector->daemons_state = realloc( detector->daemons_state, detector->failed_node_count+8);

            errmgr_set_daemon_status(temp_proc_name);
            /* increase the number of failed nodes */
            detector->failed_node_count++;
            fd_heartbeat_request(detector);
        }
    }
}

/*
 * send eager based heartbeats
 */
static int fd_heartbeat_send(prte_errmgr_detector_t* detector) {

    double now = Wtime();
    if( 0. != detector->hb_sstamp
            && (now - detector->hb_sstamp) >= 2.*detector->hb_period ) {
        /* missed my send deadline find a verbose to use */
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s MISSED my deadline by %.1e, this could trigger a false suspicion for me",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    now-detector->hb_sstamp));
    }
    detector->hb_sstamp = now;

    prte_buffer_t *buffer = NULL;
    int ret;

    buffer = PRTE_NEW(prte_buffer_t);
    prte_process_name_t daemon;
    daemon.jobid = prte_process_info.my_name.jobid;
    daemon.vpid = detector->hb_observer;
    if (PRTE_SUCCESS != (ret = prte_dss.pack(buffer, &prte_process_info.my_name.jobid, 1, PRTE_JOBID))) {
        PRTE_ERROR_LOG(ret);
    }
    if (PRTE_SUCCESS != (ret = prte_dss.pack(buffer, &prte_process_info.my_name.vpid, 1, PRTE_VPID))) {
        PRTE_ERROR_LOG(ret);
    }
    /* send the heartbeat with eager send */
    if (0 > (ret  = prte_rml.send_buffer_nb(
                    &daemon,
                    buffer,PRTE_RML_TAG_HEARTBEAT,
                    prte_rml_send_callback, NULL))) {
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector:failed to send heartbeat to %d:%d",
                    daemon.jobid, daemon.vpid));
        PRTE_ERROR_LOG(ret);
    }
    return PRTE_SUCCESS;
}

static int fd_heartbeat_recv_cb(int status, prte_process_name_t* sender,
        prte_buffer_t *buffer,
        prte_rml_tag_t tg, void *cbdata) {
    prte_errmgr_detector_t* detector = &prte_errmgr_world_detector;
    int rc;
    int32_t cnt;
    uint32_t vpid, jobid;

    if ( sender->vpid == prte_process_info.my_name.vpid)
    {
        /* this is a quit msg from observed process, stop detector */
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector:%s %s Received heartbeat from %d, which is myself, quit msg to close detector",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),__func__, sender->vpid));
        detector->hb_observing = detector->hb_observer = PRTE_VPID_INVALID;
        detector->hb_rstamp = INFINITY;
        detector->hb_period = INFINITY;
        return false;
    }

    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &jobid, &cnt, PRTE_JOBID))){
        PRTE_ERROR_LOG(rc);
    }
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &vpid, &cnt, PRTE_VPID))){
        PRTE_ERROR_LOG(rc);
    }

    if(vpid != detector->hb_observing ) {
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s receive heartbeat from vpid %d, but I am monitoring vpid %d ",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    vpid,
                    detector->hb_observing ));
    }
    else {
        double stamp = Wtime();
        double grace = detector->hb_timeout - (stamp - detector->hb_rstamp);
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s receive heartbeat from vpid %d tag %d at timestamp %g (remained %.1e of %.1e before suspecting)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    vpid,
                    tg,
                    stamp,
                    grace,
                    detector->hb_timeout));
        detector->hb_rstamp = stamp;
        if( grace < 0.0 ) {
            PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                        "errmgr:detector: daemon %s  MISSED (%.1e)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        grace));
        }
    }
    return false;
}
