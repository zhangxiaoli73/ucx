/*
 * Copyright (C) Intel Corporation, 2023-2024. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ze_ipc_iface.h"
#include "ze_ipc_ep.h"

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

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    dev_addr  = (uint64_t *)params->device_addr;
    same_uuid = (ucs_get_system_id() == *dev_addr);

    if ((getpid() == *(pid_t*)params->iface_addr) && same_uuid) {
        uct_iface_fill_info_str_buf(params, "same process");
        return 0;
    }

    if (same_uuid) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    uct_iface_fill_info_str_buf(params, "different system");
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
                                          UCT_IFACE_FLAG_EVENT_RECV;

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
    ucs_queue_iter_t iter;
    unsigned count = 0;
    ze_result_t ret;

    ucs_queue_for_each_safe(event_desc, iter, &iface->outstanding, queue) {
        ret = zeEventQueryStatus(event_desc->event);
        if (ret == ZE_RESULT_NOT_READY) {
            continue;
        }

        ucs_queue_del_iter(&iface->outstanding, iter);

        /* Close IPC handle if mapped */
        if (event_desc->mapped_addr != NULL) {
            zeMemCloseIpcHandle(iface->ze_context, event_desc->mapped_addr);
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

    return count;
}


static ucs_status_t
uct_ze_ipc_iface_flush(uct_iface_h tl_iface, unsigned flags,
                       uct_completion_t *comp)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_iface, uct_ze_ipc_iface_t);

    if (ucs_queue_is_empty(&iface->outstanding)) {
        UCT_TL_IFACE_STAT_FLUSH(ucs_derived_of(tl_iface, uct_base_iface_t));
        return UCS_OK;
    }

    UCT_TL_IFACE_STAT_FLUSH_WAIT(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_INPROGRESS;
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
    .iface_event_fd_get       = (uct_iface_event_fd_get_func_t)ucs_empty_function_return_unsupported,
    .iface_event_arm          = (uct_iface_event_arm_func_t)ucs_empty_function_return_unsupported,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_ze_ipc_iface_t),
    .iface_query              = uct_ze_ipc_iface_query,
    .iface_get_device_address = uct_ze_ipc_iface_get_device_address,
    .iface_get_address        = uct_ze_ipc_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
};


static ucs_status_t
uct_ze_ipc_estimate_perf(uct_iface_h tl_iface, uct_perf_attr_t *perf_attr)
{
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


static UCS_CLASS_INIT_FUNC(uct_ze_ipc_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_ze_ipc_iface_config_t *config;
    uct_ze_ipc_md_t *ze_md;
    ze_command_queue_desc_t queue_desc = {};
    ze_command_list_desc_t list_desc = {};
    ze_result_t ret;

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
    self->cmd_list       = NULL;
    self->config         = *config;

    /* Create command queue */
    queue_desc.ordinal = 0;
    queue_desc.mode    = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ret = zeCommandQueueCreate(self->ze_context, self->ze_device, &queue_desc,
                               &self->cmd_queue);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandQueueCreate failed with error 0x%x", ret);
        return UCS_ERR_IO_ERROR;
    }

    /* Create command list */
    ret = zeCommandListCreate(self->ze_context, self->ze_device, &list_desc,
                              &self->cmd_list);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandListCreate failed with error 0x%x", ret);
        zeCommandQueueDestroy(self->cmd_queue);
        return UCS_ERR_IO_ERROR;
    }

    ucs_queue_head_init(&self->outstanding);

    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_ze_ipc_iface_t)
{
    uct_ze_ipc_event_desc_t *event_desc;

    /* Clean up outstanding events */
    while (!ucs_queue_is_empty(&self->outstanding)) {
        event_desc = ucs_queue_pull_elem_non_empty(&self->outstanding,
                                                   uct_ze_ipc_event_desc_t,
                                                   queue);
        if (event_desc->mapped_addr != NULL) {
            zeMemCloseIpcHandle(self->ze_context, event_desc->mapped_addr);
        }
        if (event_desc->event != NULL) {
            zeEventDestroy(event_desc->event);
        }
        if (event_desc->event_pool != NULL) {
            zeEventPoolDestroy(event_desc->event_pool);
        }
        ucs_free(event_desc);
    }

    if (self->cmd_list != NULL) {
        zeCommandListDestroy(self->cmd_list);
    }
    if (self->cmd_queue != NULL) {
        zeCommandQueueDestroy(self->cmd_queue);
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
