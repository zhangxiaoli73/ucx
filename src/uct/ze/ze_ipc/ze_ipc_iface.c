/*
 * Copyright (C) Intel Corporation, 2023-2024. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ze_ipc_iface.h"
#include "ze_ipc_ep.h"
#include "ze_ipc_cache.h"

#include <uct/ze/base/ze_base.h>
#include <ucs/type/class.h>
#include <ucs/sys/string.h>
#include <ucs/debug/assert.h>
#include <ucs/async/eventfd.h>

#include <sys/types.h>
#include <unistd.h>


static ucs_config_field_t uct_ze_ipc_iface_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_ze_ipc_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"MAX_POLL", "16",
     "Max number of event completions to pick during ze events polling",
     ucs_offsetof(uct_ze_ipc_iface_config_t, max_poll),
     UCS_CONFIG_TYPE_UINT},

    {"MAX_CMD_LISTS", UCS_PP_MAKE_STRING(UCT_ZE_IPC_MAX_PEERS),
     "Max number of command lists to make concurrent progress on",
     ucs_offsetof(uct_ze_ipc_iface_config_t, max_cmd_lists),
     UCS_CONFIG_TYPE_UINT},

    {"ENABLE_CACHE", "yes",
     "Enable IPC handle caching to improve performance",
     ucs_offsetof(uct_ze_ipc_iface_config_t, enable_cache),
     UCS_CONFIG_TYPE_BOOL},

    {"BW", "50000MBs",
     "Effective p2p memory bandwidth",
     ucs_offsetof(uct_ze_ipc_iface_config_t, bandwidth),
     UCS_CONFIG_TYPE_BW},

    {"LAT", "1.8us",
     "Estimated latency",
     ucs_offsetof(uct_ze_ipc_iface_config_t, latency),
     UCS_CONFIG_TYPE_TIME},

    {"OVERHEAD", "4.0us",
     "Estimated CPU overhead for transferring GPU memory",
     ucs_offsetof(uct_ze_ipc_iface_config_t, overhead),
     UCS_CONFIG_TYPE_TIME},

    {NULL}
};


/* Forward declaration for the delete function */
static void UCS_CLASS_DELETE_FUNC_NAME(uct_ze_ipc_iface_t)(uct_iface_t*);


static ucs_status_t
uct_ze_ipc_iface_event_fd_get(uct_iface_h tl_iface, int *fd_p)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);
    ucs_status_t status;

    if (iface->eventfd == UCS_ASYNC_EVENTFD_INVALID_FD) {
        status = ucs_async_eventfd_create(&iface->eventfd);
        if (status != UCS_OK) {
            return status;
        }
    }

    *fd_p = iface->eventfd;
    return UCS_OK;
}


static ucs_status_t
uct_ze_ipc_iface_event_arm(uct_iface_h tl_iface, unsigned events)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);
    ucs_status_t status;

    /* Check if there are outstanding events that are already ready */
    if (!ucs_queue_is_empty(&iface->outstanding)) {
        uct_ze_ipc_event_desc_t *event_desc;
        ucs_queue_iter_t iter;
        ze_result_t ret;

        ucs_queue_for_each_safe(event_desc, iter, &iface->outstanding, queue) {
            ret = zeEventQueryStatus(event_desc->event);
            if (ret != ZE_RESULT_NOT_READY) {
                /* Event is ready, return BUSY to force progress */
                return UCS_ERR_BUSY;
            }
        }
    }

    /* Poll the eventfd to clear any pending signals */
    if (iface->eventfd != UCS_ASYNC_EVENTFD_INVALID_FD) {
        status = ucs_async_eventfd_poll(iface->eventfd);
        if (status == UCS_OK) {
            /* There was a pending event, return BUSY */
            return UCS_ERR_BUSY;
        } else if (status == UCS_ERR_IO_ERROR) {
            return status;
        }
    }

    return UCS_OK;
}


static ucs_status_t
uct_ze_ipc_iface_get_device_address(uct_iface_t *tl_iface,
                                    uct_device_addr_t *addr)
{
    *(uint64_t*)addr = ucs_get_system_id();
    return UCS_OK;
}


static ucs_status_t
uct_ze_ipc_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *iface_addr)
{
    *(pid_t*)iface_addr = getpid();
    return UCS_OK;
}


static int
uct_ze_ipc_iface_is_reachable_v2(const uct_iface_h tl_iface,
                                 const uct_iface_is_reachable_params_t *params)
{
    uint64_t *dev_addr;
    int same_uuid;
    int result;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        ucs_debug("ze_ipc: is_reachable_v2 - addrs not valid");
        return 0;
    }

    dev_addr  = (uint64_t *)params->device_addr;
    same_uuid = (ucs_get_system_id() == *dev_addr);

    ucs_info("ze_ipc: is_reachable_v2 - local_system_id=0x%lx remote_dev_addr=0x%lx "
             "same_uuid=%d local_pid=%d remote_pid=%d",
             (unsigned long)ucs_get_system_id(), (unsigned long)*dev_addr,
             same_uuid, getpid(), *(pid_t*)params->iface_addr);

    if ((getpid() == *(pid_t*)params->iface_addr) && same_uuid) {
        uct_iface_fill_info_str_buf(params, "same process");
        ucs_info("ze_ipc: is_reachable_v2 - rejected: same process");
        return 0;
    }

    if (same_uuid) {
        result = uct_iface_scope_is_reachable(tl_iface, params);
        ucs_info("ze_ipc: is_reachable_v2 - same system, scope_is_reachable=%d", result);
        return result;
    }

    uct_iface_fill_info_str_buf(params, "different system");
    ucs_info("ze_ipc: is_reachable_v2 - rejected: different system");
    return 0;
}

static ucs_status_t
uct_ze_ipc_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);

    uct_base_iface_query(&iface->super, iface_attr);

    iface_attr->iface_addr_len          = sizeof(pid_t);
    iface_attr->device_addr_len         = sizeof(uint64_t);
    iface_attr->ep_addr_len             = 0;
    iface_attr->max_conn_priv           = 0;
    iface_attr->cap.flags               = UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE |
                                          UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                          UCT_IFACE_FLAG_PENDING          |
                                          UCT_IFACE_FLAG_GET_ZCOPY        |
                                          UCT_IFACE_FLAG_PUT_ZCOPY;

    iface_attr->cap.event_flags         = UCT_IFACE_FLAG_EVENT_SEND_COMP |
                                          UCT_IFACE_FLAG_EVENT_RECV      |
                                          UCT_IFACE_FLAG_EVENT_FD;

    iface_attr->cap.put.max_short       = 0;
    iface_attr->cap.put.max_bcopy       = 0;
    iface_attr->cap.put.min_zcopy       = 0;
    iface_attr->cap.put.max_zcopy       = ULONG_MAX;
    iface_attr->cap.put.opt_zcopy_align = 1;
    iface_attr->cap.put.align_mtu       = iface_attr->cap.put.opt_zcopy_align;
    iface_attr->cap.put.max_iov         = 1;

    iface_attr->cap.get.max_bcopy       = 0;
    iface_attr->cap.get.min_zcopy       = 0;
    iface_attr->cap.get.max_zcopy       = ULONG_MAX;
    iface_attr->cap.get.opt_zcopy_align = 1;
    iface_attr->cap.get.align_mtu       = iface_attr->cap.get.opt_zcopy_align;
    iface_attr->cap.get.max_iov         = 1;

    iface_attr->latency                 = ucs_linear_func_make(1e-6, 0);
    iface_attr->bandwidth.dedicated     = 0;
    iface_attr->bandwidth.shared        = iface->config.bandwidth;
    iface_attr->overhead                = 7.0e-6;
    iface_attr->priority                = 0;

    return UCS_OK;
}


static unsigned
uct_ze_ipc_iface_progress(uct_iface_h tl_iface)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);
    uct_ze_ipc_event_desc_t *event_desc;
    uct_ze_ipc_queue_desc_t *q_desc;
    ucs_queue_iter_t iter;
    unsigned count = 0;
    unsigned max_poll = iface->config.max_poll;
    ze_result_t ret;

    /*
     * Progress all active command list queues
     * Similar to CUDA IPC's uct_cuda_base_progress_event_queue
     */
    ucs_queue_for_each_extract(q_desc, &iface->active_queue, queue, 1) {
        ucs_queue_for_each_safe(event_desc, iter, &q_desc->event_queue, queue) {
            /* Check if we've reached max_poll limit */
            if (count >= max_poll) {
                goto out;
            }

            ret = zeEventQueryStatus(event_desc->event);
            if (ret == ZE_RESULT_NOT_READY) {
                continue;
            }

            ucs_queue_del_iter(&q_desc->event_queue, iter);

            /* Unmap IPC handle using cache */
            if (event_desc->mapped_addr != NULL) {
                ucs_status_t status;
                status = uct_ze_ipc_unmap_memhandle(event_desc->pid,
                                                    event_desc->address,
                                                    event_desc->mapped_addr,
                                                    iface->ze_context,
                                                    event_desc->dup_fd,
                                                    iface->config.enable_cache);
                if (status != UCS_OK) {
                    ucs_warn("failed to unmap IPC handle addr:%p",
                             event_desc->mapped_addr);
                }
            }

            /* Invoke completion callback */
            if (event_desc->comp != NULL) {
                uct_invoke_completion(event_desc->comp, UCS_OK);
            }

            /* Destroy event and pool */
            zeEventDestroy(event_desc->event);
            zeEventPoolDestroy(event_desc->event_pool);
            ucs_free(event_desc);

            count++;
        }

        /* If queue still has events, put it back to active queue */
        if (!ucs_queue_is_empty(&q_desc->event_queue)) {
            ucs_queue_push(&iface->active_queue, &q_desc->queue);
        }
    }

out:
    return count;
}


static ucs_status_t
uct_ze_ipc_iface_flush(uct_iface_h tl_iface, unsigned flags,
                       uct_completion_t *comp)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);
    unsigned i;

    /* Check if all command list queues are empty */
    for (i = 0; i < iface->num_cmd_lists; i++) {
        if (!ucs_queue_is_empty(&iface->queue_desc[i].event_queue)) {
            UCT_TL_IFACE_STAT_FLUSH_WAIT(ucs_derived_of(tl_iface, uct_base_iface_t));
            return UCS_INPROGRESS;
        }
    }

    UCT_TL_IFACE_STAT_FLUSH(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_OK;
}


static uct_iface_ops_t uct_ze_ipc_iface_ops = {
    .ep_get_zcopy             = uct_ze_ipc_ep_get_zcopy,
    .ep_put_zcopy             = uct_ze_ipc_ep_put_zcopy,
    .ep_pending_add           = (uct_ep_pending_add_func_t)ucs_empty_function_return_busy,
    .ep_pending_purge         = (uct_ep_pending_purge_func_t)ucs_empty_function,
    .ep_flush                 = uct_base_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_unsupported,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_ze_ipc_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_ze_ipc_ep_t),
    .iface_flush              = uct_ze_ipc_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_ze_ipc_iface_progress,
    .iface_event_fd_get       = uct_ze_ipc_iface_event_fd_get,
    .iface_event_arm          = uct_ze_ipc_iface_event_arm,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_ze_ipc_iface_t),
    .iface_query              = uct_ze_ipc_iface_query,
    .iface_get_device_address = uct_ze_ipc_iface_get_device_address,
    .iface_get_address        = uct_ze_ipc_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
};


static ucs_status_t
uct_ze_ipc_estimate_perf(uct_iface_h tl_iface, uct_perf_attr_t *perf_attr)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH) {
        perf_attr->bandwidth.dedicated = 0;
        perf_attr->bandwidth.shared    = iface->config.bandwidth;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_PATH_BANDWIDTH) {
        perf_attr->path_bandwidth.dedicated = 0;
        perf_attr->path_bandwidth.shared    = iface->config.bandwidth;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_PRE_OVERHEAD) {
        perf_attr->send_pre_overhead = iface->config.overhead;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_POST_OVERHEAD) {
        perf_attr->send_post_overhead = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_RECV_OVERHEAD) {
        perf_attr->recv_overhead = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LATENCY) {
        perf_attr->latency = ucs_linear_func_make(iface->config.latency, 0.0);
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_MAX_INFLIGHT_EPS) {
        perf_attr->max_inflight_eps = SIZE_MAX;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_FLAGS) {
        perf_attr->flags = 0;
    }

    return UCS_OK;
}

static uct_iface_internal_ops_t uct_ze_ipc_iface_internal_ops = {
    .iface_query_v2         = uct_iface_base_query_v2,
    .iface_estimate_perf    = uct_ze_ipc_estimate_perf,
    .iface_vfs_refresh      = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .iface_mem_element_pack = (uct_iface_mem_element_pack_func_t)ucs_empty_function_return_unsupported,
    .ep_query               = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate          = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2    = (uct_ep_connect_to_ep_v2_func_t)ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2  = uct_ze_ipc_iface_is_reachable_v2,
    .ep_is_connected        = uct_ze_ipc_ep_is_connected
};


/* Find the copy engine queue group ordinal for a device */
static ucs_status_t
uct_ze_ipc_find_copy_ordinal(ze_device_handle_t device, uint32_t *ordinal_p,
                              uint32_t *num_queues_p)
{
    ze_command_queue_group_properties_t queue_props[16];
    uint32_t num_queue_groups = 16;
    uint32_t i;
    ze_result_t ret;

    /* Initialize structure types */
    for (i = 0; i < 16; i++) {
        queue_props[i].stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES;
        queue_props[i].pNext = NULL;
    }

    /* Get queue group properties */
    ret = zeDeviceGetCommandQueueGroupProperties(device, &num_queue_groups,
                                                  queue_props);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("ze_ipc_iface: zeDeviceGetCommandQueueGroupProperties failed: 0x%x",
                  ret);
        return UCS_ERR_IO_ERROR;
    }

    /* First pass: find dedicated copy engine (COPY flag but NOT COMPUTE) */
    for (i = 0; i < num_queue_groups; i++) {
        if ((queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) &&
            !(queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE)) {
            *ordinal_p = i;
            *num_queues_p = queue_props[i].numQueues;
            ucs_debug("ze_ipc_iface: using dedicated copy engine queue group %u (numQueues=%u)",
                      i, queue_props[i].numQueues);
            return UCS_OK;
        }
    }

    /* Second pass: find any queue group that supports COPY */
    for (i = 0; i < num_queue_groups; i++) {
        if (queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) {
            *ordinal_p = i;
            *num_queues_p = queue_props[i].numQueues;
            ucs_debug("ze_ipc_iface: using copy-capable queue group %u (numQueues=%u)",
                      i, queue_props[i].numQueues);
            return UCS_OK;
        }
    }

    ucs_error("ze_ipc_iface: no copy-capable queue group found");
    return UCS_ERR_NO_DEVICE;
}

static UCS_CLASS_INIT_FUNC(uct_ze_ipc_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_ze_ipc_iface_config_t *config;
    uct_ze_ipc_md_t *ze_md;
    ze_command_queue_desc_t queue_desc = {};
    uint32_t copy_ordinal = 0;
    uint32_t num_queues = 0;
    ucs_status_t status;
    ze_result_t ret;
    unsigned i;

    config = ucs_derived_of(tl_config, uct_ze_ipc_iface_config_t);
    ze_md  = ucs_derived_of(md, uct_ze_ipc_md_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_ze_ipc_iface_ops,
                              &uct_ze_ipc_iface_internal_ops, md, worker,
                              params, tl_config
                              UCS_STATS_ARG(params->stats_root)
                              UCS_STATS_ARG(UCT_ZE_IPC_TL_NAME));

    self->ze_context     = ze_md->ze_context;
    self->ze_device      = ze_md->ze_device;
    self->cmd_queue      = NULL;
    self->cmd_list       = NULL;  /* deprecated, kept for compatibility */
    self->config         = *config;
    self->eventfd        = UCS_ASYNC_EVENTFD_INVALID_FD;
    self->next_cmd_list  = 0;

    /* Find copy engine queue group ordinal and available queue count */
    status = uct_ze_ipc_find_copy_ordinal(self->ze_device, &copy_ordinal, &num_queues);
    if (status != UCS_OK) {
        return status;
    }

    /* Validate and adjust max_cmd_lists based on hardware capabilities */
    if (config->max_cmd_lists > UCT_ZE_IPC_MAX_PEERS) {
        ucs_error("ze_ipc_iface: invalid max_cmd_lists value (%u > %u)",
                  config->max_cmd_lists, UCT_ZE_IPC_MAX_PEERS);
        return UCS_ERR_INVALID_PARAM;
    }

    /*
     * Create multiple immediate command lists for parallel execution.
     *
     * Key insight from Level Zero specification:
     * - For immediate command lists, queue_desc.index MUST be 0
     * - Multiple immediate command lists can be created with the same index=0
     * - The driver automatically manages parallel execution across hardware queues
     * - Each immediate command list is independent and can execute concurrently
     *
     * This is different from regular command queues where different index values
     * (0, 1, 2...) can be used to explicitly select different hardware queues.
     */
    self->num_cmd_lists = config->max_cmd_lists;
    ucs_info("ze_ipc_iface: creating %u immediate command list(s) on queue group %u "
             "(numQueues=%u, driver manages parallel execution)",
             self->num_cmd_lists, copy_ordinal, num_queues);

    /* Initialize active queue for command lists with pending operations */
    ucs_queue_head_init(&self->active_queue);
    ucs_queue_head_init(&self->outstanding);  /* deprecated, kept for compatibility */

    /*
     * Create multiple immediate command lists for parallel progress.
     *
     * According to Level Zero spec, for immediate command lists:
     * - queue_desc.index MUST be 0 (not 1, 2, 3...)
     * - Multiple immediate command lists with index=0 are allowed
     * - Driver manages parallel execution automatically
     *
     * Benefits of multiple immediate command lists:
     * - Round-robin distribution of IPC copy operations
     * - Reduced contention on single command list
     * - Better hardware utilization for concurrent operations
     * - Independent event queues for each command list
     */
    queue_desc.stype    = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
    queue_desc.ordinal  = copy_ordinal;
    queue_desc.mode     = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    queue_desc.index    = 0;  /* MUST be 0 for immediate command lists */
    queue_desc.flags    = 0;
    queue_desc.priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL;

    for (i = 0; i < self->num_cmd_lists; i++) {
        ret = zeCommandListCreateImmediate(self->ze_context, self->ze_device,
                                           &queue_desc, &self->queue_desc[i].cmd_list);
        if (ret != ZE_RESULT_SUCCESS) {
            unsigned j;
            ucs_error("ze_ipc_iface: zeCommandListCreateImmediate[%u] failed with error 0x%x",
                      i, ret);
            /* Clean up previously created command lists */
            for (j = 0; j < i; j++) {
                zeCommandListDestroy(self->queue_desc[j].cmd_list);
            }
            return UCS_ERR_IO_ERROR;
        }

        /* Initialize event queue for this command list */
        ucs_queue_head_init(&self->queue_desc[i].event_queue);

        ucs_debug("ze_ipc_iface: created immediate command list %u/%u: %p",
                  i + 1, self->num_cmd_lists, self->queue_desc[i].cmd_list);
    }

    /* For backward compatibility, set first command list as default */
    self->cmd_list = self->queue_desc[0].cmd_list;

    ucs_info("ze_ipc_iface: initialized iface for device %p context %p "
             "(pid=%d, copy_ordinal=%u, num_cmd_lists=%u)",
             self->ze_device, self->ze_context, getpid(), copy_ordinal,
             self->num_cmd_lists);

    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_ze_ipc_iface_t)
{
    uct_ze_ipc_event_desc_t *event_desc;
    unsigned i;

    /* Clean up outstanding events from all command list queues */
    for (i = 0; i < self->num_cmd_lists; i++) {
        while (!ucs_queue_is_empty(&self->queue_desc[i].event_queue)) {
            event_desc = ucs_queue_pull_elem_non_empty(&self->queue_desc[i].event_queue,
                                                       uct_ze_ipc_event_desc_t,
                                                       queue);
            if (event_desc->mapped_addr != NULL) {
                zeMemCloseIpcHandle(self->ze_context, event_desc->mapped_addr);
            }
            if (event_desc->dup_fd >= 0) {
                close(event_desc->dup_fd);
            }
            if (event_desc->event != NULL) {
                zeEventDestroy(event_desc->event);
            }
            if (event_desc->event_pool != NULL) {
                zeEventPoolDestroy(event_desc->event_pool);
            }
            ucs_free(event_desc);
        }
    }

    /* Clean up deprecated outstanding queue for backward compatibility */
    while (!ucs_queue_is_empty(&self->outstanding)) {
        event_desc = ucs_queue_pull_elem_non_empty(&self->outstanding,
                                                   uct_ze_ipc_event_desc_t,
                                                   queue);
        if (event_desc->mapped_addr != NULL) {
            zeMemCloseIpcHandle(self->ze_context, event_desc->mapped_addr);
        }
        if (event_desc->dup_fd >= 0) {
            close(event_desc->dup_fd);
        }
        if (event_desc->event != NULL) {
            zeEventDestroy(event_desc->event);
        }
        if (event_desc->event_pool != NULL) {
            zeEventPoolDestroy(event_desc->event_pool);
        }
        ucs_free(event_desc);
    }

    /* Destroy all command lists */
    for (i = 0; i < self->num_cmd_lists; i++) {
        if (self->queue_desc[i].cmd_list != NULL) {
            zeCommandListDestroy(self->queue_desc[i].cmd_list);
        }
    }
    /* cmd_queue is not used with immediate command lists */

    /* Close eventfd if created */
    if (self->eventfd != UCS_ASYNC_EVENTFD_INVALID_FD) {
        close(self->eventfd);
    }
}


static ucs_status_t
uct_ze_ipc_query_devices(uct_md_h uct_md, uct_tl_device_resource_t **tl_devices_p,
                         unsigned *num_tl_devices_p)
{
    return uct_ze_base_query_devices_common(uct_md, UCT_DEVICE_TYPE_SHM,
                                            tl_devices_p, num_tl_devices_p);
}


UCS_CLASS_DEFINE(uct_ze_ipc_iface_t, uct_base_iface_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_ze_ipc_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                          const uct_iface_params_t*, const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_ze_ipc_iface_t, uct_iface_t);

UCT_TL_DEFINE(&uct_ze_ipc_component, ze_ipc,
              uct_ze_ipc_query_devices, uct_ze_ipc_iface_t, "ZE_IPC_",
              uct_ze_ipc_iface_config_table, uct_ze_ipc_iface_config_t);
