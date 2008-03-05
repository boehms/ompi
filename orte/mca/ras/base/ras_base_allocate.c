/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/class/opal_list.h"
#include "opal/util/show_help.h"

#include "opal/dss/dss.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/hostfile/hostfile.h"
#include "orte/util/dash_host/dash_host.h"
#include "orte/util/sys_info.h"

#include "orte/mca/ras/base/ras_private.h"

/*
 * Function for selecting one component from all those that are
 * available.
 */
int orte_ras_base_allocate(orte_job_t *jdata)
{
    int rc;
    opal_list_t nodes;
    orte_node_t *node;
    orte_std_cntr_t i;
    bool override_oversubscribed;
    orte_app_context_t **apps;

    OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                         "%s ras:base:allocate",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* if we already did this, don't do it again - the pool of
     * global resources is set. 
     */
    if (orte_ras_base.allocation_read) {
        
        OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                             "%s ras:base:allocate allocation already read",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        return ORTE_SUCCESS;
    }
    
    /* Otherwise, we have to create
     * the initial set of resources that will delineate all
     * further operations serviced by this HNP. This list will
     * contain ALL nodes that can be used by any subsequent job.
     *
     * In other words, if a node isn't found in this step, then
     * no job launched by this HNP will be able to utilize it.
     */
    
    /* note that the allocation has been read so we don't
     * come in here again!
     */
    orte_ras_base.allocation_read = true;

    /* construct a list to hold the results */
    OBJ_CONSTRUCT(&nodes, opal_list_t);

    /* if a component was selected, then we know we are in a managed
     * environment.  - the active module will return a list of what it found
     */
    if (NULL != orte_ras_base.active_module)  {
        /* read the allocation */
        if (ORTE_SUCCESS != (rc = orte_ras_base.active_module->allocate(&nodes))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&nodes);
            return rc;
        }
    } 
    /* If something came back, save it and we are done */
    if (!opal_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
        * list items
        */
        if (ORTE_SUCCESS != (rc = orte_ras_base_node_insert(&nodes, jdata))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&nodes);
            return rc;
        }
        OBJ_DESTRUCT(&nodes);
        return ORTE_SUCCESS;
    }
    
    
    
    OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                         "%s ras:base:allocate nothing found in module - proceeding to hostfile",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* nothing was found, or no active module was alive. Our next
     * option is to look for a hostfile and assign our global
     * pool from there. First, we check for a default hostfile
     * as set by an mca param
     */
    if (NULL != orte_default_hostfile) {
        OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             orte_default_hostfile));
        
        /* a default hostfile was provided - parse it */
        if (ORTE_SUCCESS != (rc = orte_util_add_hostfile_nodes(&nodes,
                                                               &override_oversubscribed,
                                                               orte_default_hostfile))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&nodes);
            return rc;
        }
    }
    /* if something was found in the default hostfile, we use that as our global
     * pool - set it and we are done
     */
    if (!opal_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (ORTE_SUCCESS != (rc = orte_ras_base_node_insert(&nodes, jdata))) {
            ORTE_ERROR_LOG(rc);
        }
        /* update the jdata object with override_oversubscribed flag */
        jdata->oversubscribe_override = override_oversubscribed;
        /* cleanup */
        OBJ_DESTRUCT(&nodes);
        return rc;
    }
    
    /* Individual hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in hostfiles from across all app_contexts
     */
    
    /* convenience def */
    apps = (orte_app_context_t**)jdata->apps->addr;
    
    for (i=0; i < jdata->num_apps; i++) {
        if (NULL != apps[i]->hostfile) {
            
            OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                                 "%s ras:base:allocate checking hostfile %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 apps[i]->hostfile));
            
            /* hostfile was specified - parse it and add it to the list */
            if (ORTE_SUCCESS != (rc = orte_util_add_hostfile_nodes(&nodes,
                                                &override_oversubscribed,
                                                apps[i]->hostfile))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&nodes);
                return rc;
            }
        }
    }

    /* if something was found in the hostfile(s), we use that as our global
     * pool - set it and we are done
     */
    if (!opal_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (ORTE_SUCCESS != (rc = orte_ras_base_node_insert(&nodes, jdata))) {
            ORTE_ERROR_LOG(rc);
        }
        /* update the jdata object with override_oversubscribed flag */
        jdata->oversubscribe_override = override_oversubscribed;
        /* cleanup */
        OBJ_DESTRUCT(&nodes);
        return rc;
    }
    
    
    
    OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                         "%s ras:base:allocate nothing found in hostfiles - checking dash-host options",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* Our next option is to look for hosts provided via the -host
     * command line option. If they are present, we declare this
     * to represent not just a mapping, but to define the global
     * resource pool in the absence of any other info.
     *
     * -host lists are provided as part of the app_contexts for
     * this job. We therefore need to retrieve the app_contexts
     * for the job, and then cycle through them to see if anything
     * is there. The parser will add the -host nodes to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * by -host across all app_contexts
     */
    for (i=0; i < jdata->num_apps; i++) {
        if (0 < apps[i]->num_map) {
            if (ORTE_SUCCESS != (rc = orte_util_add_dash_host_nodes(&nodes,
                                                    &override_oversubscribed,
                                                    apps[i]->dash_host))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&nodes);
                return rc;
            }
        }
    }

    /* if something was found in -host, we use that as our global
     * pool - set it and we are done
     */
    if (!opal_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (ORTE_SUCCESS != (rc = orte_ras_base_node_insert(&nodes, jdata))) {
            ORTE_ERROR_LOG(rc);
        }
        /* update the jdata object with override_oversubscribed flag */
        jdata->oversubscribe_override = override_oversubscribed;
        /* cleanup */
        OBJ_DESTRUCT(&nodes);
        return rc;
    }
    
    
    
    OPAL_OUTPUT_VERBOSE((5, orte_ras_base.ras_output,
                         "%s ras:base:allocate nothing found in dash-host - inserting current node",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* if nothing was found by any of the above methods, then we have no
     * earthly idea what to do - so just add the local host
     */
    node = OBJ_NEW(orte_node_t);
    if (NULL == node) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        OBJ_DESTRUCT(&nodes);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }
    /* use the same name we got in orte_system_info so we avoid confusion in
     * the session directories
     */
    node->name = strdup(orte_system_info.nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->slots_inuse = 0;
    node->slots_max = 0;
    node->slots = 1;
    /* indicate that we don't know anything about over_subscribing */
    jdata->oversubscribe_override = true;
    opal_list_append(&nodes, &node->super);
    
    /* store the results in the global resource pool - this removes the
     * list items
     */
    if (ORTE_SUCCESS != (rc = orte_ras_base_node_insert(&nodes, jdata))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&nodes);
        return rc;
    }
    OBJ_DESTRUCT(&nodes);
    return ORTE_SUCCESS;
}
