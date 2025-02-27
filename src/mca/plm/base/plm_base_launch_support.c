/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */
#include <ctype.h>

#include "src/include/hash_string.h"
#include "src/util/argv.h"
#include "src/util/prte_environ.h"
#include "src/util/printf.h"
#include "src/class/prte_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/util/dash_host/dash_host.h"
#include "src/util/nidmap.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/rtc/rtc.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"
#include "src/util/hostfile/hostfile.h"
#include "src/mca/odls/odls_types.h"

#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/base/base.h"

void prte_plm_base_set_slots(prte_node_t *node)
{
    if (0 == strncmp(prte_set_slots, "cores", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_CORE, 0);
        }
    } else if (0 == strncmp(prte_set_slots, "sockets", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            if (0 == (node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                       HWLOC_OBJ_SOCKET, 0))) {
                /* some systems don't report sockets - in this case,
                 * use numanodes */
                node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                 HWLOC_OBJ_NODE, 0);
            }
        }
    } else if (0 == strncmp(prte_set_slots, "numas", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_NODE, 0);
        }
    } else if (0 == strncmp(prte_set_slots, "hwthreads", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_PU, 0L);
        }
    } else {
        /* must be a number */
        node->slots = strtol(prte_set_slots, NULL, 10);
    }
    /* mark the node as having its slots "given" */
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
}

void prte_plm_base_daemons_reported(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_topology_t *t;
    prte_node_t *node;
    int i;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* if we are not launching, then we just assume that all
     * daemons share our topology */
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) &&
        PMIX_CHECK_NSPACE(caddy->jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
        t = node->topology;
        for (i=1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t;
            }
            node->state = PRTE_NODE_STATE_UP;
        }
    }

    /* if this is an unmanaged allocation, then set the default
     * slots on each node as directed or using default
     */
    if (!prte_managed_allocation) {
        if (NULL != prte_set_slots &&
            0 != strncmp(prte_set_slots, "none", strlen(prte_set_slots))) {
            caddy->jdata->total_slots_alloc = 0;
            for (i=0; i < prte_node_pool->size; i++) {
                if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                    continue;
                }
                if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s plm:base:setting slots for node %s by %s",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, prte_set_slots));
                    prte_plm_base_set_slots(node);
                }
                caddy->jdata->total_slots_alloc += node->slots;
            }
        }
    } else {
        /* for managed allocations, the total slots allocated is fixed at time of allocation */
        caddy->jdata->total_slots_alloc = prte_ras_base.total_slots_alloc;
    }

    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DISPLAY_ALLOC, NULL, PMIX_BOOL)) {
        prte_ras_base_display_alloc(caddy->jdata);
    }
    /* ensure we update the routing plan */
    prte_routed.update_routing_plan();

    /* progress the job */
    caddy->jdata->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_VM_READY);

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_plm_base_allocation_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* if we don't want to launch, then we at least want
     * to map so we can see where the procs would have
     * gone - so skip to the mapping state */
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
    } else {
        /* move the state machine along */
        caddy->jdata->state = PRTE_JOB_STATE_ALLOCATION_COMPLETE;
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_LAUNCH_DAEMONS);
    }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_plm_base_daemons_launched(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* do NOT increment the state - we wait for the
     * daemons to report that they have actually
     * started before moving to the right state
     */
    /* cleanup */
    PRTE_RELEASE(caddy);
}

static void files_ready(int status, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t*)cbdata;

    if (PRTE_SUCCESS != status) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    } else {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    }
}

void prte_plm_base_vm_ready(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* progress the job */
    caddy->jdata->state = PRTE_JOB_STATE_VM_READY;

    /* position any required files */
    if (PRTE_SUCCESS != prte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_plm_base_mapping_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* move the state machine along */
    caddy->jdata->state = PRTE_JOB_STATE_MAP_COMPLETE;
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_SYSTEM_PREP);

    /* cleanup */
    PRTE_RELEASE(caddy);
}


void prte_plm_base_setup_job(int fd, short args, void *cbdata)
{
    int rc;
    int i;
    prte_app_context_t *app;
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:setup_job",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (PRTE_JOB_STATE_INIT != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* start by getting a jobid */
    if (PMIX_NSPACE_INVALID(caddy->jdata->nspace)) {
        if (PRTE_SUCCESS != (rc = prte_plm_base_create_jobid(caddy->jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
            PRTE_RELEASE(caddy);
            return;
        }

        /* store it on the global job data pool - this is the key
         * step required before we launch the daemons. It allows
         * the prte_rmaps_base_setup_virtual_machine routine to
         * search all apps for any hosts to be used by the vm
         *
         * Note that the prte_plm_base_create_jobid function will
         * place the "caddy->jdata" object at the correct position
         * in the hash table. There is no need to store it again here.
         */
    }

    /* if job recovery is not enabled, set it to default */
    if (!PRTE_FLAG_TEST(caddy->jdata, PRTE_JOB_FLAG_RECOVERABLE) &&
        prte_enable_recovery) {
        PRTE_FLAG_SET(caddy->jdata, PRTE_JOB_FLAG_RECOVERABLE);
    }

    /* if app recovery is not defined, set apps to defaults */
    for (i=0; i < caddy->jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(caddy->jdata->apps, i))) {
            continue;
        }
        if (!prte_get_attribute(&app->attributes, PRTE_APP_RECOV_DEF, NULL, PMIX_BOOL)) {
            prte_set_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, PRTE_ATTR_LOCAL, &prte_max_restarts, PMIX_INT32);
        }
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_INIT_COMPLETE);

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_plm_base_setup_job_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* nothing to do here but move along */
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_ALLOCATE);
    PRTE_RELEASE(caddy);
}

void prte_plm_base_complete_setup(int fd, short args, void *cbdata)
{
    prte_job_t *jdata;
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_node_t *node;
    uint32_t h;
    pmix_rank_t *vptr;
    int i, rc;
    char *serial_number;
    pmix_proc_t requestor, *rptr;

    PRTE_ACQUIRE_OBJECT(caddy);

    prte_output_verbose(5, prte_plm_base_framework.framework_output,
                        "%s complete_setup on job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(caddy->jdata->nspace));

    /* bozo check */
    if (PRTE_JOB_STATE_SYSTEM_PREP != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* convenience */
    jdata = caddy->jdata;

    /* If this job is being started by me, then there is nothing
     * further we need to do as any user directives (e.g., to tie
     * off IO to /dev/null) will have been included in the launch
     * message and the IOF knows how to handle any default situation.
     * However, if this is a proxy spawn request, then the spawner
     * might be a tool that wants IO forwarded to it. If that's the
     * situation, then the job object will contain an attribute
     * indicating that request */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FWDIO_TO_TOOL, NULL, PMIX_BOOL)) {
        /* send a message to our IOF containing the requested pull */
        rptr = &requestor;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void**)&rptr, PMIX_PROC)) {
            PRTE_IOF_PROXY_PULL(jdata, rptr);
        } else {
            PRTE_IOF_PROXY_PULL(jdata, &jdata->originator);
        }
        /* the tool will PUSH its stdin, so nothing we need to do here
         * about stdin */
    }

    /* if coprocessors were detected, now is the time to
     * identify who is attached to what host - this info
     * will be shipped to the daemons in the nidmap. Someday,
     * there may be a direct way for daemons on coprocessors
     * to detect their hosts - but not today.
     */
    if (prte_coprocessors_detected) {
        /* cycle thru the nodes looking for coprocessors */
        for (i=0; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            /* if we don't have a serial number, then we are not a coprocessor */
            serial_number = NULL;
            if (!prte_get_attribute(&node->attributes, PRTE_NODE_SERIAL_NUMBER, (void**)&serial_number, PMIX_STRING)) {
                continue;
            }
            if (NULL != serial_number) {
                /* if we have a serial number, then we are a coprocessor - so
                 * compute our hash and lookup our hostid
                 */
                PRTE_HASH_STR(serial_number, h);
                free(serial_number);
                if (PRTE_SUCCESS != (rc = prte_hash_table_get_value_uint32(prte_coprocessors, h,
                                                                           (void**)&vptr))) {
                    PRTE_ERROR_LOG(rc);
                    break;
                }
                prte_set_attribute(&node->attributes, PRTE_NODE_HOSTID, PRTE_ATTR_LOCAL, vptr, PMIX_PROC_RANK);
            }
        }
    }
    /* done with the coprocessor mapping at this time */
    if (NULL != prte_coprocessors) {
        PRTE_RELEASE(prte_coprocessors);
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_LAUNCH_APPS);

    /* cleanup */
    PRTE_RELEASE(caddy);
}

/* catch timeout to allow cmds to progress */
static void timer_cb(int fd, short event, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t*)cbdata;
    prte_timer_t *timer=NULL;

    PRTE_ACQUIRE_OBJECT(jdata);

    /* declare launch failed */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START);
    jdata->exit_code = PRTE_ERR_TIMEOUT;

    if (!prte_persistent) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_TIMEOUT);
    }

    /* free event */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FAILURE_TIMER_EVENT, (void**)&timer, PMIX_POINTER)) {
        /* timer is an prte_timer_t object */
        PRTE_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_FAILURE_TIMER_EVENT);
    }
}

void prte_plm_base_launch_apps(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_daemon_cmd_flag_t command;
    int rc;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    if (PRTE_JOB_STATE_LAUNCH_APPS != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch_apps for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* pack the appropriate add_local_procs command */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FIXED_DVM, NULL, PMIX_BOOL)) {
        command = PRTE_DAEMON_DVM_ADD_PROCS;
    } else {
        command = PRTE_DAEMON_ADD_LOCAL_PROCS;
    }
    rc = PMIx_Data_pack(NULL, &jdata->launch_msg, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PRTE_RELEASE(caddy);
        return;
    }

    /* get the local launcher's required data */
    if (PRTE_SUCCESS != (rc = prte_odls.get_add_procs_data(&jdata->launch_msg, jdata->nspace))) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
    }

    PRTE_RELEASE(caddy);
    return;
}

void prte_plm_base_send_launch_msg(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_timer_t *timer;
    prte_grpcomm_signature_t *sig;
    prte_job_t *jdata;
    int rc;

    /* convenience */
    jdata = caddy->jdata;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:send launch msg for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* if we don't want to launch the apps, now is the time to leave */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        bool compressed;
        uint8_t *cmpdata = NULL;
        size_t cmplen;
        /* report the size of the launch message */
        compressed = PMIx_Data_compress((uint8_t*)jdata->launch_msg.base_ptr,
                                        jdata->launch_msg.bytes_used,
                                        &cmpdata, &cmplen);
        if (compressed) {
            prte_output(0, "LAUNCH MSG RAW SIZE: %d COMPRESSED SIZE: %d",
                        (int)jdata->launch_msg.bytes_used, (int)cmplen);
            free(cmpdata);
        } else {
            prte_output(0, "LAUNCH MSG RAW SIZE: %d", (int)jdata->launch_msg.bytes_used);
        }
        prte_never_launched = true;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALL_JOBS_COMPLETE);
        PRTE_RELEASE(caddy);
        if (NULL != cmpdata) {
            free(cmpdata);
        }
        return;
    }

    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    sig->sz = 1;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, &jdata->launch_msg))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(sig);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PRTE_RELEASE(caddy);
        return;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&jdata->launch_msg);
    PMIX_DATA_BUFFER_CONSTRUCT(&jdata->launch_msg);
    /* maintain accounting */
    PRTE_RELEASE(sig);

    /* track that we automatically are considered to have reported - used
     * only to report launch progress
     */
    caddy->jdata->num_daemons_reported++;

    /* if requested, setup a timer - if we don't launch within the
     * defined time, then we know things have failed
     */
    if (0 < prte_startup_timeout) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch defining timeout for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_JOBID_PRINT(jdata->nspace)));
        timer = PRTE_NEW(prte_timer_t);
        timer->payload = jdata;
        prte_event_evtimer_set(prte_event_base,
                               timer->ev, timer_cb, jdata);
        prte_event_set_priority(timer->ev, PRTE_ERROR_PRI);
        timer->tv.tv_sec = prte_startup_timeout;
        timer->tv.tv_usec = 0;
        prte_set_attribute(&jdata->attributes, PRTE_JOB_FAILURE_TIMER_EVENT, PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
        PRTE_POST_OBJECT(timer);
        prte_event_evtimer_add(timer->ev, &timer->tv);
    }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

int prte_plm_base_spawn_reponse(int32_t status, prte_job_t *jdata)
{
    int rc;
    pmix_data_buffer_t *answer;
    int room, *rmptr;

    /* if the requestor simply told us to terminate, they won't
     * be waiting for a response */
    if (PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        return PRTE_SUCCESS;
    }

    /* if the response has already been sent, don't do it again */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED, NULL, PMIX_BOOL)) {
        return PRTE_SUCCESS;
    }

    /* if the requestor was a tool, use PMIx to notify them of
     * launch complete as they won't be listening on PRRTE oob */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, NULL, PMIX_BOOL)) {
        pmix_info_t *iptr;
        time_t timestamp;
        pmix_proc_t *nptr;

        /* dvm job => launch was requested by a TOOL, so we notify the launch proxy
         * and NOT the originator (as that would be us) */
        nptr = NULL;
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void**)&nptr, PMIX_PROC)
            || NULL == nptr) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }

        /* direct an event back to our controller */
        timestamp = time(NULL);
        PMIX_INFO_CREATE(iptr, 4);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, nptr, PMIX_PROC);
        PMIX_PROC_RELEASE(nptr);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[1], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
        /* not to be delivered to a default event handler */
        PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        /* provide the timestamp */
        PMIX_INFO_LOAD(&iptr[3], PMIX_EVENT_TIMESTAMP, &timestamp, PMIX_TIME);
        PMIx_Notify_event(PMIX_LAUNCH_COMPLETE, &prte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          iptr, 4, NULL, NULL);
        PMIX_INFO_FREE(iptr, 4);
    }

    /* prep the response to the spawn requestor */
    PMIX_DATA_BUFFER_CREATE(answer);

    /* pack the status */
    rc = PMIx_Data_pack(NULL, answer, &status, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return prte_pmix_convert_status(rc);
    }
    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, answer, &jdata->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return prte_pmix_convert_status(rc);
    }
    /* pack the room number */
    rmptr = &room;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_ROOM_NUM, (void**)&rmptr, PMIX_INT)) {
        rc = PMIx_Data_pack(NULL, answer, &room, 1, PMIX_INT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(answer);
            return prte_pmix_convert_status(rc);
        }
    }
    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch sending dyn release of job %s to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace),
                         PRTE_NAME_PRINT(&jdata->originator)));
    if (0 > (rc = prte_rml.send_buffer_nb(&jdata->originator, answer,
                                           PRTE_RML_TAG_LAUNCH_RESP,
                                           prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(answer);
        return rc;
    }

    /* mark that we sent it */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
    return PRTE_SUCCESS;
}

static uint32_t ntraces = 0;

static void stack_trace_recv(int status, pmix_proc_t* sender,
                             pmix_data_buffer_t *buffer, prte_rml_tag_t tag,
                             void* cbdata)
{
    pmix_byte_object_t pbo;
    pmix_data_buffer_t blob;
    char *st;
    int32_t cnt;
    pmix_proc_t name;
    char *hostname, *nspace;
    pid_t pid;
    prte_job_t *jdata = NULL;
    prte_timer_t *timer;
    prte_proc_t *proc;
    prte_pointer_array_t parray;
    int rc;

    prte_output_verbose(5, prte_plm_base_framework.framework_output,
                        "%s: stacktrace recvd from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the stack_trace blob */
    cnt = 1;
    while (PMIX_SUCCESS == PMIx_Data_unpack(NULL, buffer, &nspace, &cnt, PMIX_STRING)) {
        if (NULL == jdata) {
            jdata = prte_get_job_data_object(nspace);
        }
        free(nspace);

        if (PMIX_SUCCESS != PMIx_Data_unpack(NULL, buffer, &pbo, &cnt, PMIX_BYTE_OBJECT)) {
            continue;
        }
        PMIx_Data_load(&blob, &pbo);
        /* first piece is the name of the process */
        cnt = 1;
        if (PMIX_SUCCESS != PMIx_Data_unpack(NULL, &blob, &name, &cnt, PMIX_PROC) ||
            PMIX_SUCCESS != PMIx_Data_unpack(NULL, &blob, &hostname, &cnt, PMIX_STRING) ||
            PMIX_SUCCESS != PMIx_Data_unpack(NULL, &blob, &pid, &cnt, PMIX_PID)) {
            PMIX_DATA_BUFFER_DESTRUCT(&blob);
            continue;
        }
        fprintf(stderr, "STACK TRACE FOR PROC %s (%s, PID %lu)\n", PRTE_NAME_PRINT(&name), hostname, (unsigned long) pid);
        free(hostname);
        /* unpack the stack_trace until complete */
        cnt = 1;
        while (PRTE_SUCCESS == PMIx_Data_unpack(NULL, &blob, &st, &cnt, PMIX_STRING)) {
            fprintf(stderr, "\t%s", st);  // has its own newline
            free(st);
            cnt = 1;
        }
        fprintf(stderr, "\n");
        PMIX_DATA_BUFFER_DESTRUCT(&blob);
        cnt = 1;
    }
    ++ntraces;
    if (prte_process_info.num_daemons == ntraces) {
        timer = NULL;
        if (NULL != jdata &&
            prte_get_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT, (void**)&timer, PMIX_POINTER) &&
            NULL != timer) {
            prte_event_evtimer_del(timer->ev);
            /* timer is an prte_timer_t object */
            PRTE_RELEASE(timer);
            prte_remove_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT);
        }
        /* abort the job */
        PRTE_CONSTRUCT(&parray, prte_pointer_array_t);
        /* create an object */
        proc = PRTE_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&proc->name, jdata->nspace, PMIX_RANK_WILDCARD);
        cnt = prte_pointer_array_add(&parray, proc);
        if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
            PRTE_ERROR_LOG(rc);
        }
        PRTE_RELEASE(proc);
        prte_pointer_array_set_item(&parray, cnt, NULL);
        PRTE_DESTRUCT(&parray);
        ntraces = 0;
    }
}

static void stack_trace_timeout(int sd, short args, void *cbdata)
{
    prte_timer_t *timer;
    prte_job_t *jdata = (prte_job_t*)cbdata;
    prte_proc_t *proc;
    prte_pointer_array_t parray;
    int rc;

    /* clear the timer */
    timer = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, (void**)&timer, PMIX_POINTER) &&
        NULL != timer) {
        prte_event_evtimer_del(timer->ev);
        /* timer is an prte_timer_t object */
        PRTE_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT);
    }

    /* abort the job */
    PRTE_CONSTRUCT(&parray, prte_pointer_array_t);
    /* create an object */
    proc = PRTE_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pointer_array_add(&parray, proc);
    if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
        PRTE_ERROR_LOG(rc);
    }
    return;
    PRTE_RELEASE(proc);
    PRTE_DESTRUCT(&parray);
    PRTE_RELEASE(jdata);
}

/* catch job execution timeout */
static void timeout_cb(int fd, short event, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t*)cbdata;
    prte_timer_t *timer=NULL;
    prte_proc_t *proc;
    int i, rc, timeout, *tp;
    prte_pointer_array_t parray;

    PRTE_ACQUIRE_OBJECT(jdata);

    /* Display a useful message to the user */
    tp = &timeout;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT,
                             (void**)&tp, PMIX_INT)) {
        /* This shouldn't happen, but at least don't segv / display
           *something* if it does */
        timeout = -1;
    }
    prte_show_help("help-plm-base.txt", "timeout",
                    true, timeout);
    PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_TIMEOUT);

    /* see if they want proc states reported */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_STATE, NULL, PMIX_BOOL)) {
        /* don't use the opal_output system as it may be borked */
        fprintf(stderr, "DATA FOR JOB: %s\n", PRTE_JOBID_PRINT(jdata->nspace));
        fprintf(stderr, "\tNum apps: %d\tNum procs: %d\tJobState: %s\tAbort: %s\n",
                (int)jdata->num_apps, (int)jdata->num_procs,
                prte_job_state_to_str(jdata->state),
                (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) ? "True" : "False");
        fprintf(stderr, "\tNum launched: %ld\tNum reported: %ld\tNum terminated: %ld\n",
                (long)jdata->num_launched, (long)jdata->num_reported, (long)jdata->num_terminated);
        fprintf(stderr, "\n\tProcs:\n");
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL != (proc = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, i))) {
                fprintf(stderr, "\t\tRank: %s\tNode: %s\tPID: %u\tState: %s\tExitCode %d\n",
                        PRTE_VPID_PRINT(proc->name.rank),
                        (NULL == proc->node) ? "UNKNOWN" : proc->node->name,
                        (unsigned int)proc->pid,
                        prte_proc_state_to_str(proc->state), proc->exit_code);
            }
        }
        fprintf(stderr, "\n");
    }

    /* see if they want stacktraces */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_STACKTRACES, NULL, PMIX_BOOL)) {
        /* if they asked for stack_traces, attempt to get them, but timeout
         * if we cannot do so */
        prte_daemon_cmd_flag_t command = PRTE_DAEMON_GET_STACK_TRACES;
        pmix_data_buffer_t buffer;
        prte_grpcomm_signature_t *sig;

        fprintf(stderr, "Waiting for stack traces (this may take a few moments)...\n");

        /* set the recv */
        prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_STACK_TRACE,
                                 PRTE_RML_PERSISTENT, stack_trace_recv, NULL);

        /* setup the buffer */
        PMIX_DATA_BUFFER_CONSTRUCT(&buffer);
        /* pack the command */
        rc = PMIx_Data_pack(NULL, &buffer, &command, 1, PMIX_UINT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&buffer);
            goto giveup;
        }
        /* pack the jobid */
        rc = PMIx_Data_pack(NULL, &buffer, &jdata->nspace, 1, PMIX_PROC_NSPACE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&buffer);
            goto giveup;
        }
        /* goes to all daemons */
        sig = PRTE_NEW(prte_grpcomm_signature_t);
        sig->signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
        PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
        sig->sz = 1;
        if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, &buffer))) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&buffer);
            goto giveup;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&buffer);
        /* maintain accounting */
        PRTE_RELEASE(sig);
        /* we will terminate after we get the stack_traces, but set a timeout
         * just in case we never hear back from everyone */
        if (prte_stack_trace_wait_timeout > 0) {
            timer = PRTE_NEW(prte_timer_t);
            prte_event_evtimer_set(prte_event_base,
                                    timer->ev, stack_trace_timeout, jdata);
            timer->tv.tv_sec = prte_stack_trace_wait_timeout;
            timer->tv.tv_usec = 0;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT, PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
            PRTE_POST_OBJECT(timer);
            prte_event_evtimer_add(timer->ev, &timer->tv);
        }
        return;
    }

  giveup:
    /* abort the job */
    PRTE_CONSTRUCT(&parray, prte_pointer_array_t);
    /* create an object */
    proc = PRTE_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pointer_array_add(&parray, proc);
    if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
        PRTE_ERROR_LOG(rc);
    }
    PRTE_RELEASE(proc);
    PRTE_DESTRUCT(&parray);
}

void prte_plm_base_post_launch(int fd, short args, void *cbdata)
{
    int32_t rc;
    prte_job_t *jdata;
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_timer_t *timer=NULL;
    int time, *tp;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    /* if a timer was defined, cancel it */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FAILURE_TIMER_EVENT, (void**)&timer, PMIX_POINTER)) {
        prte_event_evtimer_del(timer->ev);
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch deleting timeout for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_JOBID_PRINT(jdata->nspace)));
        PRTE_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_FAILURE_TIMER_EVENT);
    }

    if (PRTE_JOB_STATE_RUNNING != caddy->job_state) {
        /* error mgr handles this */
        PRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* complete wiring up the iof */
    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch wiring up iof for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* notify the spawn requestor */
    rc = prte_plm_base_spawn_reponse(PRTE_SUCCESS, jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* if the job has a timeout assigned to it, setup the timer for it */
    tp = &time;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT, (void**)&tp, PMIX_INT)) {
        /* setup a timer to monitor execution time */
        timer = PRTE_NEW(prte_timer_t);
        timer->payload = jdata;
        prte_event_evtimer_set(prte_event_base,
                               timer->ev, timeout_cb, jdata);
        prte_event_set_priority(timer->ev, PRTE_ERROR_PRI);
        timer->tv.tv_sec = time;
        timer->tv.tv_usec = 0;
        prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
        PRTE_POST_OBJECT(timer);
        prte_event_evtimer_add(timer->ev, &timer->tv);
    }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_plm_base_registered(int fd, short args, void *cbdata)
{
    prte_job_t *jdata;
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch %s registered",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    if (PRTE_JOB_STATE_REGISTERED != caddy->job_state) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch job %s not registered - state %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_JOBID_PRINT(jdata->nspace),
                             prte_job_state_to_str(caddy->job_state)));
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FORCED_EXIT);
        PRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    jdata->state = caddy->job_state;

    PRTE_RELEASE(caddy);
}

/* daemons callback when they start - need to listen for them */
static bool prted_failed_launch;
static prte_job_t *jdatorted=NULL;

/* callback for topology reports */
void prte_plm_base_daemon_topology(int status, pmix_proc_t* sender,
                                   pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tag, void *cbdata)
{
    hwloc_topology_t topo;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *sum;
    int rc, idx;
    char *sig, *coprocessors, **sns;
    prte_proc_t *daemon=NULL;
    prte_topology_t *t, *t2;
    int i;
    uint32_t h;
    prte_job_t *jdata;
    uint8_t flag;
    pmix_data_buffer_t datbuf, *data;
    pmix_byte_object_t bo, pbo;
    pmix_topology_t ptopo;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:daemon_topology recvd for daemon %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    }
    if (NULL == (daemon = (prte_proc_t*)prte_pointer_array_get_item(jdatorted->procs, sender->rank))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&datbuf);
    /* unpack the flag to see if this payload is compressed */
    idx=1;
    rc = PMIx_Data_unpack(NULL, buffer, &flag, &idx, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    /* unpack the data */
    idx = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &pbo, &idx, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    /* if compressed, decompress it */
    if (flag) {
        /* decompress the data */
        if (PMIx_Data_decompress((uint8_t**)&bo.bytes, &bo.size,
                                 (uint8_t*)pbo.bytes, pbo.size)) {
            /* the data has been uncompressed */
            rc = PMIx_Data_load(&datbuf, &bo);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        } else {
            PMIX_ERROR_LOG(PMIX_ERROR);
            prted_failed_launch = true;
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            goto CLEANUP;
        }
    } else {
        rc = PMIx_Data_load(&datbuf, &pbo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    data = &datbuf;

    /* unpack the topology signature for this node */
    idx=1;
    rc = PMIx_Data_unpack(NULL, data, &sig, &idx, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    /* find it in the array */
    t = NULL;
    for (i=0; i < prte_node_topologies->size; i++) {
        if (NULL == (t2 = (prte_topology_t*)prte_pointer_array_get_item(prte_node_topologies, i))) {
            continue;
        }
        /* just check the signature */
        if (0 == strcmp(sig, t2->sig)) {
            t = t2;
            break;
        }
    }
    if (NULL == t) {
        /* should never happen */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prted_failed_launch = true;
        goto CLEANUP;
    }

    /* unpack the topology */
    idx=1;
    rc = PMIx_Data_unpack(NULL, data, &ptopo, &idx, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    topo = ptopo.topology;
    ptopo.topology = NULL;
    PMIX_TOPOLOGY_DESTRUCT(&ptopo);
    /* Apply any CPU filters (not preserved by the XML) */
    prte_hwloc_base_filter_cpus(topo);
    /* record the final topology */
    t->topo = topo;
    /* setup the summary data for this topology as we will need
     * it when we go to map/bind procs to it */
    root = hwloc_get_root_obj(topo);
    root->userdata = (void*)PRTE_NEW(prte_hwloc_topo_data_t);
    sum = (prte_hwloc_topo_data_t*)root->userdata;
    sum->available = prte_hwloc_base_setup_summary(topo);

    /* unpack any coprocessors */
    idx=1;
    rc = PMIx_Data_unpack(NULL, data, &coprocessors, &idx, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    if (NULL != coprocessors) {
        /* init the hash table, if necessary */
        if (NULL == prte_coprocessors) {
            prte_coprocessors = PRTE_NEW(prte_hash_table_t);
            prte_hash_table_init(prte_coprocessors, prte_process_info.num_daemons);
        }
        /* separate the serial numbers of the coprocessors
         * on this host
         */
        sns = prte_argv_split(coprocessors, ',');
        for (idx=0; NULL != sns[idx]; idx++) {
            /* compute the hash */
            PRTE_HASH_STR(sns[idx], h);
            /* mark that this coprocessor is hosted by this node */
            prte_hash_table_set_value_uint32(prte_coprocessors, h, (void*)&daemon->name.rank);
        }
        prte_argv_free(sns);
        free(coprocessors);
        prte_coprocessors_detected = true;
    }
    /* see if this daemon is on a coprocessor */
    idx=1;
    rc = PMIx_Data_unpack(NULL, data, &coprocessors, &idx, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    if (NULL != coprocessors) {
        if (prte_get_attribute(&daemon->node->attributes, PRTE_NODE_SERIAL_NUMBER, NULL, PMIX_STRING)) {
            /* this is not allowed - a coprocessor cannot be host
             * to another coprocessor at this time
             */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
            prted_failed_launch = true;
            free(coprocessors);
            goto CLEANUP;
        }
        prte_set_attribute(&daemon->node->attributes, PRTE_NODE_SERIAL_NUMBER, PRTE_ATTR_LOCAL, coprocessors, PMIX_STRING);
        free(coprocessors);
        prte_coprocessors_detected = true;
    }

  CLEANUP:
    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:orted:report_topo launch %s for daemon %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         prted_failed_launch ? "failed" : "completed",
                         PRTE_NAME_PRINT(sender)));

    if (prted_failed_launch) {
        PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_FAILED_TO_START);
        return;
    } else {
        jdatorted->num_reported++;
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch recvd %d of %d reported daemons",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             jdatorted->num_reported, jdatorted->num_procs));
        if (jdatorted->num_procs == jdatorted->num_reported) {
            bool dvm = true;
            jdatorted->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
            /* activate the daemons_reported state for all jobs
             * whose daemons were launched
             */
            for (i=1; i < prte_job_data->size; i++) {
                jdata = (prte_job_t*)prte_pointer_array_get_item(prte_job_data, i);
                if (NULL == jdata) {
                    continue;
                }
                if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                    dvm = false;
                    if (PRTE_JOB_STATE_DAEMONS_LAUNCHED == jdata->state) {
                        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
                    }
                }
            }
            if (dvm) {
                /* must be launching a DVM - activate the state */
                PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_DAEMONS_REPORTED);
            }
        }
    }
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

void prte_plm_base_daemon_callback(int status, pmix_proc_t* sender,
                                   pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tag, void *cbdata)
{
    char *ptr;
    int idx;
    pmix_status_t ret;
    prte_proc_t *daemon=NULL;
    prte_job_t *jdata;
    pmix_proc_t dname;
    pmix_data_buffer_t *relay;
    char *sig;
    prte_topology_t *t, *mytopo;
    hwloc_topology_t topo;
    int i;
    bool found;
    prte_daemon_cmd_flag_t cmd;
    char *myendian;
    char *alias, **atmp;
    uint8_t naliases, ni;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *sum;
    char *nodename = NULL;
    pmix_info_t *info;
    size_t n, ninfo;
    pmix_byte_object_t pbo, bo;
    pmix_data_buffer_t pbuf;
    int32_t flag;
    bool compressed;
    pmix_data_buffer_t datbuf, *data;
    pmix_topology_t ptopo;

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    }

    /* get my endianness */
    mytopo = (prte_topology_t*)prte_pointer_array_get_item(prte_node_topologies, 0);
    if (NULL == mytopo) {
        /* should never happen */
        myendian = "unknown";
    } else {
        myendian = strrchr(mytopo->sig, ':');
        ++myendian;
    }

    /* multiple daemons could be in this buffer, so unpack until we exhaust the data */
    idx = 1;
    while (PMIX_SUCCESS == (ret = PMIx_Data_unpack(NULL, buffer, &dname, &idx, PMIX_PROC))) {

        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch from daemon %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&dname)));

        atmp = NULL;
        /* update state and record for this daemon contact info */
        if (NULL == (daemon = (prte_proc_t*)prte_pointer_array_get_item(jdatorted->procs, dname.rank))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        daemon->state = PRTE_PROC_STATE_RUNNING;
        /* record that this daemon is alive */
        PRTE_FLAG_SET(daemon, PRTE_PROC_FLAG_ALIVE);

        /* unpack the flag indicating if we have info objects */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &flag, &idx, PMIX_INT32);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }

        if (0 < flag) {
            /* unpack the byte object containing the info array */
            idx = 1;
            ret = PMIx_Data_unpack(NULL, buffer, &pbo, &idx, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* load the bytes into a PMIx data buffer for unpacking */
            PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
            ret = PMIx_Data_load(&pbuf, &pbo);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            /* unpack the number of info structs */
            idx = 1;
            ret = PMIx_Data_unpack(NULL, &pbuf, &ninfo, &idx, PMIX_SIZE);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            PMIX_INFO_CREATE(info, ninfo);
            idx = ninfo;
            ret = PMIx_Data_unpack(NULL, &pbuf, info, &idx, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_INFO_FREE(info, ninfo);
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);

            for (n=0; n < ninfo; n++) {
                /* store this in a daemon wireup buffer for later distribution */
                if (PMIX_SUCCESS != (ret = PMIx_Store_internal(&dname, info[n].key, &info[n].value))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
            }
            PMIX_INFO_FREE(info, ninfo);
        }

        /* unpack the node name */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &nodename, &idx, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        if (!prte_have_fqdn_allocation) {
            /* remove any domain info */
            if (NULL != (ptr = strchr(nodename, '.'))) {
                *ptr = '\0';
                ptr = strdup(nodename);
                free(nodename);
                nodename = ptr;
            }
        }

        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch from daemon %s on node %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&daemon->name), nodename));

        /* mark the daemon as launched */
        PRTE_FLAG_SET(daemon->node, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
        daemon->node->state = PRTE_NODE_STATE_UP;

        /* first, store the nodename itself as an alias. We do
         * this in case the nodename isn't the same as what we
         * were given by the allocation. For example, a hostfile
         * might contain an IP address instead of the value returned
         * by gethostname, yet the daemon will have returned the latter
         * and apps may refer to the host by that name
         */
        prte_argv_append_nosize(&atmp, nodename);
        /* unpack and store the provided aliases */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &naliases, &idx, PMIX_UINT8);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        for (ni=0; ni < naliases; ni++) {
            idx = 1;
            ret = PMIx_Data_unpack(NULL, buffer, &alias, &idx, PMIX_STRING);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            prte_argv_append_nosize(&atmp, alias);
            free(alias);
        }
        if (0 < naliases) {
            alias = prte_argv_join(atmp, ',');
            prte_set_attribute(&daemon->node->attributes, PRTE_NODE_ALIAS, PRTE_ATTR_LOCAL, alias, PMIX_STRING);
            free(alias);
        }
        prte_argv_free(atmp);

        /* unpack the topology signature for that node */
        idx=1;
        ret = PMIx_Data_unpack(NULL, buffer, &sig, &idx, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s RECEIVED TOPOLOGY SIG %s FROM NODE %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sig, nodename));

        if (NULL == prte_base_compute_node_sig) {
            prte_base_compute_node_sig = strdup(sig);
            if (prte_hnp_is_allocated && 0 != strcmp(sig, mytopo->sig)) {
                prte_hetero_nodes = true;
            }
        } else if (!prte_hetero_nodes) {
            if (0 != strcmp(sig, prte_base_compute_node_sig) ||
                (prte_hnp_is_allocated && 0 != strcmp(sig, mytopo->sig))) {
                prte_hetero_nodes = true;
            }
        }

        /* rank=1 always sends its topology back */
        topo = NULL;
        if (1 == dname.rank) {
            PMIX_DATA_BUFFER_CONSTRUCT(&datbuf);
            /* unpack the flag to see if this payload is compressed */
            idx=1;
            ret = PMIx_Data_unpack(NULL, buffer, &compressed, &idx, PMIX_BOOL);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* unpack the data */
            idx=1;
            ret = PMIx_Data_unpack(NULL, buffer, &pbo, &idx, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* only need to process it if our signatures differ */
            if (0 == strcmp(sig, mytopo->sig)) {
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            } else {
                if (compressed) {
                    /* decompress the data */
                    if (PMIx_Data_decompress((uint8_t**)&bo.bytes, &bo.size,
                                             (uint8_t*)pbo.bytes, pbo.size)) {
                        /* the data has been uncompressed */
                        ret = PMIx_Data_load(&datbuf, &bo);
                        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                        if (PMIX_SUCCESS != ret) {
                            PMIX_ERROR_LOG(ret);
                            prted_failed_launch = true;
                            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                            goto CLEANUP;
                        }
                    } else {
                        PMIX_ERROR_LOG(PMIX_ERROR);
                        prted_failed_launch = true;
                        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                        goto CLEANUP;
                    }
                } else {
                    ret = PMIx_Data_load(&datbuf, &pbo);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        prted_failed_launch = true;
                        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                        goto CLEANUP;
                    }
                }
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                data = &datbuf;

                 /* unpack the available topology information */
                idx=1;
                ret = PMIx_Data_unpack(NULL, data, &ptopo, &idx, PMIX_TOPO);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                topo = ptopo.topology;
                ptopo.topology = NULL;
                PMIX_TOPOLOGY_DESTRUCT(&ptopo);
                /* setup the summary data for this topology as we will need
                 * it when we go to map/bind procs to it */
                root = hwloc_get_root_obj(topo);
                root->userdata = (void*)PRTE_NEW(prte_hwloc_topo_data_t);
                sum = (prte_hwloc_topo_data_t*)root->userdata;
                sum->available = prte_hwloc_base_setup_summary(topo);
                /* cleanup */
                PMIX_DATA_BUFFER_DESTRUCT(data);
            }
        }

        /* see if they provided their inventory */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &flag, &idx, PMIX_INT8);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        if (1 == flag) {
            ret = PMIx_Data_unpack(NULL, buffer, &pbo, &idx, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* if nothing is present, then ignore it */
            if (0 < pbo.size) {
                prte_pmix_lock_t lock;
                /* load the bytes into a PMIx data buffer for unpacking */
                PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
                ret = PMIx_Data_load(&pbuf, &pbo);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                idx = 1;
                ret = PMIx_Data_unpack(NULL, &pbuf, &ninfo, &idx, PMIX_SIZE);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PMIX_INFO_CREATE(info, ninfo);
                idx = ninfo;
                ret = PMIx_Data_unpack(NULL, &pbuf, info, &idx, PMIX_INFO);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                PRTE_PMIX_CONSTRUCT_LOCK(&lock);
                ret = PMIx_server_deliver_inventory(info, ninfo, NULL, 0, opcbfunc, &lock);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PRTE_PMIX_WAIT_THREAD(&lock);
                PRTE_PMIX_DESTRUCT_LOCK(&lock);
            }
        }

        /* do we already have this topology from some other node? */
        found = false;
        for (i=0; i < prte_node_topologies->size; i++) {
            if (NULL == (t = (prte_topology_t*)prte_pointer_array_get_item(prte_node_topologies, i))) {
                continue;
            }
            /* just check the signature */
            if (0 == strcmp(sig, t->sig)) {
                PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                     "%s TOPOLOGY ALREADY RECORDED",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                found = true;
                daemon->node->topology = t;
                if (NULL != topo) {
                    hwloc_topology_destroy(topo);
                }
                free(sig);
                break;
            }
        }

        if (!found) {
            /* nope - save the signature and request the complete topology from that node */
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s NEW TOPOLOGY - ADDING",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            t = PRTE_NEW(prte_topology_t);
            t->sig = sig;
            t->index = prte_pointer_array_add(prte_node_topologies, t);
            daemon->node->topology = t;
            if (NULL != topo) {
                /* Apply any CPU filters (not preserved by the XML) */
                prte_hwloc_base_filter_cpus(topo);
                t->topo = topo;
            } else {
                PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                     "%s REQUESTING TOPOLOGY FROM %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&dname)));
                /* construct the request */
                PMIX_DATA_BUFFER_CREATE(relay);
                cmd = PRTE_DAEMON_REPORT_TOPOLOGY_CMD;
                ret = PMIx_Data_pack(NULL, relay, &cmd, 1, PMIX_UINT8);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_RELEASE(relay);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                /* send it */
                prte_rml.send_buffer_nb(&dname, relay,
                                        PRTE_RML_TAG_DAEMON,
                                        prte_rml_send_callback, NULL);
                /* we will count this node as completed
                 * when we get the full topology back */
                if (NULL != nodename) {
                    free(nodename);
                    nodename = NULL;
                }
                idx = 1;
                continue;
            }
        }

      CLEANUP:
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch %s for daemon %s at contact %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             prted_failed_launch ? "failed" : "completed",
                             PRTE_NAME_PRINT(&dname),
                             (NULL == daemon) ? "UNKNOWN" : daemon->rml_uri));

        if (NULL != nodename) {
            free(nodename);
            nodename = NULL;
        }

        if (prted_failed_launch) {
            PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_FAILED_TO_START);
            return;
        } else {
            jdatorted->num_reported++;
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:orted_report_launch job %s recvd %d of %d reported daemons",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(jdatorted->nspace),
                                 jdatorted->num_reported, jdatorted->num_procs));
            if (jdatorted->num_procs == jdatorted->num_reported) {
                bool dvm = true;
                jdatorted->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
                /* activate the daemons_reported state for all jobs
                 * whose daemons were launched
                 */
                for (i=1; i < prte_job_data->size; i++) {
                    jdata = (prte_job_t*)prte_pointer_array_get_item(prte_job_data, i);
                    if (NULL == jdata) {
                        continue;
                    }
                    if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                        dvm = false;
                        if (PRTE_JOB_STATE_DAEMONS_LAUNCHED == jdata->state) {
                            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
                        }
                    }
                }
                if (dvm) {
                    /* must be launching a DVM - activate the state */
                    PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_DAEMONS_REPORTED);
                }
            }
        }
        idx = 1;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_FAILED_TO_START);
    }
}

void prte_plm_base_daemon_failed(int st, pmix_proc_t* sender,
                                 pmix_data_buffer_t *buffer,
                                 prte_rml_tag_t tag, void *cbdata)
{
    int status, rc;
    int32_t n;
    pmix_rank_t vpid;
    prte_proc_t *daemon=NULL;

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    }

    /* unpack the daemon that failed */
    n=1;
    rc = PMIx_Data_unpack(NULL, buffer, &vpid, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
        goto finish;
    }

    /* unpack the exit status */
    n=1;
    rc = PMIx_Data_unpack(NULL, buffer, &status, &n, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = PRTE_ERROR_DEFAULT_EXIT_CODE;
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
    } else {
        PRTE_UPDATE_EXIT_STATUS(WEXITSTATUS(status));
    }

    /* find the daemon and update its state/status */
    if (NULL == (daemon = (prte_proc_t*)prte_pointer_array_get_item(jdatorted->procs, vpid))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto finish;
    }
    daemon->state = PRTE_PROC_STATE_FAILED_TO_START;
    daemon->exit_code = status;

  finish:
    if (NULL == daemon) {
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_PROC_STATE_FAILED_TO_START);
        return;
    } else {
        PRTE_ACTIVATE_PROC_STATE(&daemon->name, PRTE_PROC_STATE_FAILED_TO_START);
    }
}

int prte_plm_base_setup_prted_cmd(int *argc, char ***argv)
{
    int i, loc;
    char **tmpv;

    /* set default location to be 0, indicating that
     * only a single word is in the cmd
     */
    loc = 0;
    /* split the command apart in case it is multi-word */
    tmpv = prte_argv_split(prte_launch_agent, ' ');
    for (i = 0; NULL != tmpv && NULL != tmpv[i]; ++i) {
        if (0 == strcmp(tmpv[i], "prted")) {
            loc = i;
        }
        prte_argv_append(argc, argv, tmpv[i]);
    }
    prte_argv_free(tmpv);

    return loc;
}


/* pass all options as MCA params so anything we pickup
 * from the environment can be checked for duplicates
 */
int prte_plm_base_prted_append_basic_args(int *argc, char ***argv,
                                          char *ess,
                                          int *proc_vpid_index)
{
    char *param = NULL;
    int i, j, cnt;
    prte_job_t *jdata;
    unsigned long num_procs;
    bool ignore;

    /* check for debug flags */
    if (prte_debug_flag) {
        prte_argv_append(argc, argv, "--debug");
    }
    if (prte_debug_daemons_flag) {
        prte_argv_append(argc, argv, "--debug-daemons");
    }
    if (prte_debug_daemons_file_flag) {
        prte_argv_append(argc, argv, "--debug-daemons-file");
    }
    if (prte_leave_session_attached) {
        prte_argv_append(argc, argv, "--leave-session-attached");
    }

    if (prte_map_stddiag_to_stderr) {
        prte_argv_append(argc, argv, "--prtemca");
        prte_argv_append(argc, argv, "prte_map_stddiag_to_stderr");
        prte_argv_append(argc, argv, "1");
    }
    else if (prte_map_stddiag_to_stdout) {
        prte_argv_append(argc, argv, "--prtemca");
        prte_argv_append(argc, argv, "prte_map_stddiag_to_stdout");
        prte_argv_append(argc, argv, "1");
    }

    /* the following is not an mca param */
    if (NULL != getenv("PRTE_TEST_PRTED_SUICIDE")) {
        prte_argv_append(argc, argv, "--test-suicide");
    }

    /* tell the orted what ESS component to use */
    if (NULL != ess) {
        prte_argv_append(argc, argv, "--prtemca");
        prte_argv_append(argc, argv, "ess");
        prte_argv_append(argc, argv, ess);
    }

    /* pass the daemon nspace */
    prte_argv_append(argc, argv, "--prtemca");
    prte_argv_append(argc, argv, "ess_base_nspace");
    prte_argv_append(argc, argv, prte_process_info.myproc.nspace);
    free(param);

    /* setup to pass the vpid */
    if (NULL != proc_vpid_index) {
        prte_argv_append(argc, argv, "--prtemca");
        prte_argv_append(argc, argv, "ess_base_vpid");
        *proc_vpid_index = *argc;
        prte_argv_append(argc, argv, "<template>");
    }

    /* pass the total number of daemons that will be in the system */
    if (PRTE_PROC_IS_MASTER) {
        jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        num_procs = jdata->num_procs;
    } else {
        num_procs = prte_process_info.num_daemons;
    }
    prte_argv_append(argc, argv, "--prtemca");
    prte_argv_append(argc, argv, "ess_base_num_procs");
    prte_asprintf(&param, "%lu", num_procs);
    prte_argv_append(argc, argv, param);
    free(param);

    /* pass the HNP uri */
    prte_argv_append(argc, argv, "--prtemca");
    prte_argv_append(argc, argv, "prte_hnp_uri");
    prte_argv_append(argc, argv, prte_process_info.my_hnp_uri);

    /* if --xterm was specified, pass that along */
    if (NULL != prte_xterm) {
        prte_argv_append(argc, argv, "--prtemca");
        prte_argv_append(argc, argv, "prte_xterm");
        prte_argv_append(argc, argv, prte_xterm);
    }

    /* pass along any cmd line MCA params provided to mpirun,
     * being sure to "purge" any that would cause problems
     * on backend nodes and ignoring all duplicates
     */
    cnt = prte_argv_count(prted_cmd_line);
    for (i=0; i < cnt; i+=3) {
        /* if the specified option is more than one word, we don't
         * have a generic way of passing it as some environments ignore
         * any quotes we add, while others don't - so we ignore any
         * such options. In most cases, this won't be a problem as
         * they typically only apply to things of interest to the HNP.
         * Individual environments can add these back into the cmd line
         * as they know if it can be supported
         */
        if (NULL != strchr(prted_cmd_line[i+2], ' ')) {
            continue;
        }
        /* The daemon will attempt to open the PLM on the remote
         * end. Only a few environments allow this, so the daemon
         * only opens the PLM -if- it is specifically told to do
         * so by giving it a specific PLM module. To ensure we avoid
         * confusion, do not include any directives here
         */
        if (0 == strcmp(prted_cmd_line[i+1], "plm")) {
            continue;
        }
        /* check for duplicate */
        ignore = false;
        for (j=0; j < *argc; j++) {
            if (0 == strcmp((*argv)[j], prted_cmd_line[i+1])) {
                ignore = true;
                break;
            }
        }
        if (!ignore) {
            /* pass it along */
            prte_argv_append(argc, argv, prted_cmd_line[i]);
            prte_argv_append(argc, argv, prted_cmd_line[i+1]);
            prte_argv_append(argc, argv, prted_cmd_line[i+2]);
        }
    }

    return PRTE_SUCCESS;
}

void prte_plm_base_wrap_args(char **args)
{
    int i;
    char *tstr;

    for (i=0; NULL != args && NULL != args[i]; i++) {
        /* if the arg ends in "mca", then we wrap its arguments */
        if (strlen(args[i]) > 3 && 0 == strcmp(args[i] + strlen(args[i]) - 3, "mca")) {
            /* it was at the end */
            if (NULL == args[i+1] || NULL == args[i+2]) {
                /* this should be impossible as the error would
                 * have been detected well before here, but just
                 * be safe */
                return;
            }
            i += 2;
            /* if the argument already has quotes, then leave it alone */
            if ('\"' == args[i][0]) {
                continue;
            }
            prte_asprintf(&tstr, "\"%s\"", args[i]);
            free(args[i]);
            args[i] = tstr;
        }
    }
}

int prte_plm_base_setup_virtual_machine(prte_job_t *jdata)
{
    prte_node_t *node, *nptr;
    prte_proc_t *proc, *pptr;
    prte_job_map_t *map=NULL;
    int rc, i;
    prte_job_t *daemons;
    prte_list_t nodes, tnodes;
    prte_list_item_t *item, *next;
    prte_app_context_t *app;
    bool one_filter = false;
    int num_nodes;
    bool default_hostfile_used;
    char *hosts = NULL;
    bool singleton=false;
    bool multi_sim = false;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:setup_vm",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (NULL == (daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    if (NULL == daemons->map) {
        daemons->map = PRTE_NEW(prte_job_map_t);
    }
    map = daemons->map;

    /* if this job is being launched against a fixed DVM, then there is
     * nothing for us to do - the DVM will stand as is */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FIXED_DVM, NULL, PMIX_BOOL)) {
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        map->num_new_daemons = 0;
        return PRTE_SUCCESS;
    }

    /* if this is a dynamic spawn, then we don't make any changes to
     * the virtual machine unless specifically requested to do so
     */
    if (!PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        if (0 == map->num_nodes) {
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm creating map",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* this is the first time thru, so the vm is just getting
             * defined - create a map for it and put us in as we
             * are obviously already here! The ess will already
             * have assigned our node to us.
             */
            node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
            if (NULL == node) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return PRTE_ERR_NOT_FOUND;
            }
            prte_pointer_array_add(map->nodes, (void*)node);
            ++(map->num_nodes);
            /* maintain accounting */
            PRTE_RETAIN(node);
            /* mark that this is from a singleton */
            singleton = true;
        }
        PRTE_CONSTRUCT(&nodes, prte_list_t);
        for (i=1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            /* only add in nodes marked as "added" */
            if (!singleton && PRTE_NODE_STATE_ADDED != node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "%s plm_base:setup_vm NODE %s WAS NOT ADDED",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
                continue;
            }
            PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                 "%s plm_base:setup_vm ADDING NODE %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRTE_RETAIN(node);
            prte_list_append(&nodes, &node->super);
            /* reset the state so it can be used for mapping */
            node->state = PRTE_NODE_STATE_UP;
        }
        map->num_new_daemons = 0;
        /* if we didn't get anything, then there is nothing else to
         * do as no other daemons are to be launched
         */
        if (0 == prte_list_get_size(&nodes)) {
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm no new daemons required",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
            PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
            return PRTE_SUCCESS;
        }
        /* if we got some new nodes to launch, we need to handle it */
        goto process;
    }

    /* if we are not working with a virtual machine, then we
     * look across all jobs and ensure that the "VM" contains
     * all nodes with application procs on them
     */
    multi_sim = prte_get_attribute(&jdata->attributes, PRTE_JOB_MULTI_DAEMON_SIM, NULL, PMIX_BOOL);
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_NO_VM, NULL, PMIX_BOOL) || multi_sim) {
        PRTE_CONSTRUCT(&nodes, prte_list_t);
        /* loop across all nodes and include those that have
         * num_procs > 0 && no daemon already on them
         */
        for (i=1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRTE_NODE_STATE_UP;
                continue;
            }
            if (PRTE_NODE_STATE_DOWN == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            if (0 < node->num_procs || multi_sim) {
                /* retain a copy for our use in case the item gets
                 * destructed along the way
                 */
                PRTE_RETAIN(node);
                prte_list_append(&nodes, &node->super);
            }
        }
        if (multi_sim) {
            goto process;
        }
        /* see if anybody had procs */
        if (0 == prte_list_get_size(&nodes)) {
            /* if the HNP has some procs, then we are still good */
            node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
            if (NULL == node) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return PRTE_ERR_NOT_FOUND;
            }
            if (0 < node->num_procs) {
                PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                     "%s plm:base:setup_vm only HNP in use",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PRTE_DESTRUCT(&nodes);
                map->num_nodes = 1;
                /* mark that the daemons have reported so we can proceed */
                daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
                return PRTE_SUCCESS;
            }
            /* well, if the HNP doesn't have any procs, and neither did
             * anyone else...then we have a big problem
             */
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            return PRTE_ERR_FATAL;
        }
        goto process;
    }

    if (0 == map->num_nodes) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm creating map",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* this is the first time thru, so the vm is just getting
         * defined - put us in as we
         * are obviously already here! The ess will already
         * have assigned our node to us.
         */
        node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }
        prte_pointer_array_add(map->nodes, (void*)node);
        ++(map->num_nodes);
        /* maintain accounting */
        PRTE_RETAIN(node);
    }

    /* zero-out the number of new daemons as we will compute this
     * each time we are called
     */
    map->num_new_daemons = 0;

    /* setup the list of nodes */
    PRTE_CONSTRUCT(&nodes, prte_list_t);

    /* if this is an unmanaged allocation, then we use
     * the nodes that were specified for the union of
     * all apps - there is no need to collect all
     * available nodes and "filter" them
     */
    if (!prte_managed_allocation) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s setup:vm: working unmanaged allocation",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        default_hostfile_used = false;
        PRTE_CONSTRUCT(&tnodes, prte_list_t);
        hosts = NULL;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void**)&hosts, PMIX_STRING)) {
            /* use the file, if provided */
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s using rank/seqfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 hosts));
            if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&tnodes, hosts))) {
                PRTE_ERROR_LOG(rc);
                free(hosts);
                return rc;
            }
            free(hosts);
        } else {
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                /* if the app provided a dash-host, then use those nodes */
                hosts = NULL;
                if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void**)&hosts, PMIX_STRING)) {
                    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s using dash_host",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&tnodes, hosts, false))) {
                        PRTE_ERROR_LOG(rc);
                        free(hosts);
                        return rc;
                    }
                    free(hosts);
                } else if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void**)&hosts, PMIX_STRING)) {
                    /* otherwise, if the app provided a hostfile, then use that */
                    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s using hostfile %s",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));
                    if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&tnodes, hosts))) {
                        PRTE_ERROR_LOG(rc);
                        free(hosts);
                        return rc;
                    }
                    free(hosts);
                } else if (NULL != prte_default_hostfile) {
                    if (!default_hostfile_used) {
                        /* fall back to the default hostfile, if provided */
                        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                             "%s using default hostfile %s",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                             prte_default_hostfile));
                        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&tnodes,
                                                                               prte_default_hostfile))) {
                            PRTE_ERROR_LOG(rc);
                            return rc;
                        }
                        /* only include it once */
                        default_hostfile_used = true;
                    }
                }
            }
        }

        /* cycle thru the resulting list, finding the nodes on
         * the node pool array while removing ourselves
         * and all nodes that are down or otherwise unusable
         */
        while (NULL != (item = prte_list_remove_first(&tnodes))) {
            nptr = (prte_node_t*)item;
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s checking node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 nptr->name));
            for (i=0; i < prte_node_pool->size; i++) {
                if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                    continue;
                }
                if (0 != strcmp(node->name, nptr->name)) {
                    continue;
                }
                /* have a match - now see if we want this node */
                /* ignore nodes that are marked as do-not-use for this mapping */
                if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_USE", node->name));
                    /* reset the state so it can be used another time */
                    node->state = PRTE_NODE_STATE_UP;
                    break;
                }
                if (PRTE_NODE_STATE_DOWN == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED DOWN", node->name));
                    break;
                }
                if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_INCLUDE", node->name));
                    break;
                }
                /* if this node is us, ignore it */
                if (0 == node->index) {
                    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s ignoring myself",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    break;
                }
                /* we want it - add it to list */
                PRTE_RETAIN(node);
                prte_list_append(&nodes, &node->super);
            }
            PRTE_RELEASE(nptr);
        }
        PRTE_LIST_DESTRUCT(&tnodes);
        /* if we didn't get anything, then we are the only node in the
         * allocation - so there is nothing else to do as no other
         * daemons are to be launched
         */
        if (0 == prte_list_get_size(&nodes)) {
            PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm only HNP in allocation",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
            PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
            return PRTE_SUCCESS;
        }
        /* continue processing */
        goto process;
    }

    /* construct a list of available nodes */
    for (i=1; i < prte_node_pool->size; i++) {
        if (NULL != (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRTE_NODE_STATE_UP;
                continue;
            }
            if (PRTE_NODE_STATE_DOWN == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRTE_RETAIN(node);
            prte_list_append(&nodes, &node->super);
            /* by default, mark these as not to be included
             * so the filtering logic works correctly
             */
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * system - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == prte_list_get_size(&nodes)) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP in allocation",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* cleanup */
        PRTE_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
        return PRTE_SUCCESS;
    }

    /* filter across the union of all app_context specs - if the HNP
     * was allocated, then we have to include
     * ourselves in case someone has specified a -host or hostfile
     * that includes the head node. We will remove ourselves later
     * as we clearly already exist
     */
    if (prte_hnp_is_allocated) {
        node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }
        PRTE_RETAIN(node);
        prte_list_prepend(&nodes, &node->super);
    }
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_filter_nodes(app, &nodes, false)) &&
            rc != PRTE_ERR_TAKE_NEXT_OPTION) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRTE_SUCCESS == rc) {
            /* we filtered something */
            one_filter = true;
        }
    }

    if (one_filter) {
        /* at least one filtering option was executed, so
         * remove all nodes that were not mapped
         */
        item = prte_list_get_first(&nodes);
        while (item != prte_list_get_end(&nodes)) {
            next = prte_list_get_next(item);
            node = (prte_node_t*)item;
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                prte_list_remove_item(&nodes, item);
                PRTE_RELEASE(item);
            } else {
                /* The filtering logic sets this flag only for nodes which
                 * are kept after filtering. This flag will be subsequently
                 * used in rmaps components and must be reset here */
                PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
            }
            item = next;
        }
    }

    /* ensure we are not on the list */
    if (0 < prte_list_get_size(&nodes)) {
        item = prte_list_get_first(&nodes);
        node = (prte_node_t*)item;
        if (0 == node->index) {
            prte_list_remove_item(&nodes, item);
            PRTE_RELEASE(item);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * allocation - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == prte_list_get_size(&nodes)) {
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP left",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
        return PRTE_SUCCESS;
    }

 process:
    /* cycle thru all available nodes and find those that do not already
     * have a daemon on them - no need to include our own as we are
     * obviously already here! If a max vm size was given, then limit
     * the overall number of active nodes to the given number. Only
     * count the HNP's node if it was included in the allocation
     */
    if (prte_hnp_is_allocated) {
        num_nodes = 1;
    } else {
        num_nodes = 0;
    }
    while (NULL != (item = prte_list_remove_first(&nodes))) {
        /* if a max size was given and we are there, then exit the loop */
        if (0 < prte_max_vm_size && num_nodes == prte_max_vm_size) {
            /* maintain accounting */
            PRTE_RELEASE(item);
            break;
        }
        node = (prte_node_t*)item;
        /* if this node is already in the map, skip it */
        if (NULL != node->daemon) {
            num_nodes++;
            /* maintain accounting */
            PRTE_RELEASE(item);
            continue;
        }
        /* add the node to the map - we retained it
         * when adding it to the list, so we don't need
         * to retain it again
         */
        prte_pointer_array_add(map->nodes, (void*)node);
        ++(map->num_nodes);
        num_nodes++;
        /* create a new daemon object for this node */
        proc = PRTE_NEW(prte_proc_t);
        if (NULL == proc) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        PMIX_LOAD_NSPACE(&proc->name, PRTE_PROC_MY_NAME->nspace);
        if (PMIX_RANK_VALID-1 <= daemons->num_procs) {
            /* no more daemons available */
            prte_show_help("help-prte-rmaps-base.txt", "out-of-vpids", true);
            PRTE_RELEASE(proc);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        proc->name.rank = daemons->num_procs;  /* take the next available vpid */
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm add new daemon %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&proc->name)));
        /* add the daemon to the daemon job object */
        if (0 > (rc = prte_pointer_array_set_item(daemons->procs, proc->name.rank, (void*)proc))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        ++daemons->num_procs;
        PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm assigning new daemon %s to node %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&proc->name),
                             node->name));
        /* point the node to the daemon */
        node->daemon = proc;
        PRTE_RETAIN(proc);  /* maintain accounting */
        /* point the proc to the node and maintain accounting */
        proc->node = node;
        PRTE_RETAIN(node);
        if (prte_plm_globals.daemon_nodes_assigned_at_launch) {
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
        } else {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
        }
        /* track number of daemons to be launched */
        ++map->num_new_daemons;
        /* and their starting vpid */
        if (PMIX_RANK_INVALID == map->daemon_vpid_start) {
            map->daemon_vpid_start = proc->name.rank;
        }
        /* loop across all app procs on this node and update their parent */
        for (i=0; i < node->procs->size; i++) {
            if (NULL != (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                pptr->parent = proc->name.rank;
            }
        }
    }

    if (prte_process_info.num_daemons != daemons->num_procs) {
        /* more daemons are being launched - update the routing tree to
         * ensure that the HNP knows how to route messages via
         * the daemon routing tree - this needs to be done
         * here to avoid potential race conditions where the HNP
         * hasn't unpacked its launch message prior to being
         * asked to communicate.
         */
        prte_process_info.num_daemons = daemons->num_procs;

        /* ensure all routing plans are up-to-date - we need this
         * so we know how to tree-spawn and/or xcast info */
        prte_routed.update_routing_plan();
    }

    /* mark that the daemon job changed */
    PRTE_FLAG_SET(daemons, PRTE_JOB_FLAG_UPDATED);

    /* if new daemons are being launched, mark that this job
     * caused it to happen */
    if (0 < map->num_new_daemons) {
        if (PRTE_SUCCESS != (rc = prte_set_attribute(&jdata->attributes, PRTE_JOB_LAUNCHED_DAEMONS,
                                                     true, NULL, PMIX_BOOL))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}
