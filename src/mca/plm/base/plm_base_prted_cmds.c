/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
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

#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif


#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_wait.h"
#include "src/prted/prted.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"

#if 0
static void failed_cmd(int fd, short event, void *cbdata)
{
    prte_timer_t *tm = (prte_timer_t*)cbdata;

    /* we get called if an abnormal term
     * don't complete in time - just force exit
     */
    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd command timed out",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    PRTE_RELEASE(tm);
    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
}
#endif

int prte_plm_base_prted_exit(prte_daemon_cmd_flag_t command)
{
    int rc;
    pmix_data_buffer_t cmd;
    prte_daemon_cmd_flag_t cmmnd;
    prte_grpcomm_signature_t *sig;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd sending orted_exit commands",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* flag that orteds are being terminated */
    prte_prteds_term_ordered = true;
    cmmnd = command;

    /* if we are terminating before launch, or abnormally
     * terminating, then the daemons may not be wired up
     * and therefore cannot depend on detecting their
     * routed children to determine termination
     */
    if (prte_abnormal_term_ordered ||
        prte_never_launched ||
        !prte_routing_is_enabled) {
        cmmnd = PRTE_DAEMON_HALT_VM_CMD;
    }

    /* send it express delivery! */
    PMIX_DATA_BUFFER_CONSTRUCT(&cmd);

    /* pack the command */
    rc = PMIx_Data_pack(NULL, &cmd, &cmmnd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&cmd);
        return rc;
    }
    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
    sig->sz = 1;
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, &cmd))) {
        PRTE_ERROR_LOG(rc);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&cmd);
    PRTE_RELEASE(sig);

#if 0
    /* if we are abnormally ordering the termination, then
     * set a timeout in case it never finishes
     */
    if (prte_abnormal_term_ordered) {
        PRTE_DETECT_TIMEOUT(prte_process_info.num_procs, 100, 3, failed_cmd, NULL);
    }
#endif
    return rc;
}


int prte_plm_base_prted_terminate_job(pmix_nspace_t jobid)
{
    prte_pointer_array_t procs;
    prte_proc_t proc;
    int rc;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:prted_terminate job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jobid)));

    PRTE_CONSTRUCT(&procs, prte_pointer_array_t);
    prte_pointer_array_init(&procs, 1, 1, 1);
    PRTE_CONSTRUCT(&proc, prte_proc_t);
    PMIX_LOAD_PROCID(&proc.name, jobid, PMIX_RANK_WILDCARD);
    prte_pointer_array_add(&procs, &proc);
    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_kill_local_procs(&procs))) {
        PRTE_ERROR_LOG(rc);
    }
    PRTE_DESTRUCT(&procs);
    PRTE_DESTRUCT(&proc);
    return rc;
}

int prte_plm_base_prted_kill_local_procs(prte_pointer_array_t *procs)
{
    int rc;
    pmix_data_buffer_t cmd;
    prte_daemon_cmd_flag_t command=PRTE_DAEMON_KILL_LOCAL_PROCS;
    int v;
    prte_proc_t *proc;
    prte_grpcomm_signature_t *sig;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd sending kill_local_procs cmds",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    PMIX_DATA_BUFFER_CONSTRUCT(&cmd);
    /* pack the command */
    rc = PMIx_Data_pack(NULL, &cmd, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&cmd);
        return rc;
    }

    /* pack the proc names */
    if (NULL != procs) {
        for (v=0; v < procs->size; v++) {
            if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(procs, v))) {
                continue;
            }
            rc = PMIx_Data_pack(NULL, &cmd, &proc->name, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&cmd);
                return rc;
            }
        }
    }
    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
    sig->sz = 1;
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, &cmd))) {
        PRTE_ERROR_LOG(rc);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&cmd);
    PRTE_RELEASE(sig);

    /* we're done! */
    return rc;
}


int prte_plm_base_prted_signal_local_procs(pmix_nspace_t job, int32_t signal)
{
    int rc;
    pmix_data_buffer_t cmd;
    prte_daemon_cmd_flag_t command=PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
    prte_grpcomm_signature_t *sig;

    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:prted_cmd sending signal_local_procs cmds",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    PMIX_DATA_BUFFER_CONSTRUCT(&cmd);

    /* pack the command */
    rc = PMIx_Data_pack(NULL, &cmd, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&cmd);
        return rc;
    }

    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, &cmd, &job, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&cmd);
        return rc;
    }

    /* pack the signal */
    rc = PMIx_Data_pack(NULL, &cmd, &signal, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&cmd);
        return rc;
    }

    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
    sig->sz = 1;
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, &cmd))) {
        PRTE_ERROR_LOG(rc);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&cmd);
    PRTE_RELEASE(sig);

    /* we're done! */
    return PRTE_SUCCESS;
}
