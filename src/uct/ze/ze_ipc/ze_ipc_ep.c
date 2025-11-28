/*
 * Copyright (C) Intel Corporation, 2023-2024. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ze_ipc_ep.h"
#include "ze_ipc_iface.h"
#include "ze_ipc_md.h"
#include <uct/ze/base/ze_base.h>

#include <uct/base/uct_log.h>
#include <uct/base/uct_iov.inl>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/type/class.h>
#include <ucs/profile/profile.h>

#define UCT_ZE_IPC_PUT 0
#define UCT_ZE_IPC_GET 1


static UCS_CLASS_INIT_FUNC(uct_ze_ipc_ep_t, const uct_ep_params_t *params)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(params->iface,
                                               uct_ze_ipc_iface_t);

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    self->remote_pid = *(const pid_t*)params->iface_addr;

    ucs_info("ze_ipc_ep: created endpoint to remote pid %d (local pid %d)",
             self->remote_pid, getpid());
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_ze_ipc_ep_t)
{
}


UCS_CLASS_DEFINE(uct_ze_ipc_ep_t, uct_base_ep_t)
UCS_CLASS_DEFINE_NEW_FUNC(uct_ze_ipc_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_ze_ipc_ep_t, uct_ep_t);


#define uct_ze_ipc_trace_data(_addr, _rkey, _fmt, ...)     \
    ucs_trace_data(_fmt " to %"PRIx64"(%+ld)", ## __VA_ARGS__, (_addr), (_rkey))


int uct_ze_ipc_ep_is_connected(const uct_ep_h tl_ep,
                               const uct_ep_is_connected_params_t *params)
{
    const uct_ze_ipc_ep_t *ep = ucs_derived_of(tl_ep, uct_ze_ipc_ep_t);

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    return ep->remote_pid == *(pid_t*)params->iface_addr;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_ze_ipc_post_copy(uct_ep_h tl_ep, uint64_t remote_addr,
                     const uct_iov_t *iov, uct_rkey_t rkey,
                     uct_completion_t *comp, int direction)
{
    uct_ze_ipc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ze_ipc_iface_t);
    uct_ze_ipc_key_t *key     = (uct_ze_ipc_key_t *)rkey;
    uct_ze_ipc_event_desc_t *event_desc;
    ze_event_pool_desc_t event_pool_desc = {};
    ze_event_desc_t event_desc_ze = {};
    void *mapped_addr = NULL;
    void *mapped_rem_addr;
    void *dst, *src;
    size_t offset;
    ze_result_t ret;

    if (ucs_unlikely(iov[0].length == 0)) {
        ucs_trace_data("Zero length request: skip it");
        return UCS_OK;
    }

    /* Print handle info for verification - should match PACK and UNPACK output */
    {
        const unsigned char *bytes = (const unsigned char *)&key->ipc_handle;
        uint32_t sum = 0;
        int local_dev_id = uct_ze_base_get_device_ordinal(iface->ze_device);
        for (size_t i = 0; i < sizeof(ze_ipc_mem_handle_t); i++) {
            sum += bytes[i];
            sum = (sum << 1) | (sum >> 31);
        }
        ucs_info("OPEN(receiver): remote_dev=%d local_dev=%d checksum=0x%08x "
                 "handle[0-15]=%02x%02x%02x%02x%02x%02x%02x%02x"
                 "%02x%02x%02x%02x%02x%02x%02x%02x",
                 key->dev_num, local_dev_id, sum,
                 bytes[0], bytes[1], bytes[2], bytes[3],
                 bytes[4], bytes[5], bytes[6], bytes[7],
                 bytes[8], bytes[9], bytes[10], bytes[11],
                 bytes[12], bytes[13], bytes[14], bytes[15]);
    }

    ucs_info("ze_ipc_ep: post_copy direction=%s remote_addr=0x%lx length=%zu "
             "local_device=%p (id=%d) context=%p",
             (direction == UCT_ZE_IPC_PUT) ? "PUT" : "GET",
             (unsigned long)remote_addr, iov[0].length,
             (void*)iface->ze_device,
             uct_ze_base_get_device_ordinal(iface->ze_device),
             (void*)iface->ze_context);

    /* Open IPC handle to get mapped address */
    ret = zeMemOpenIpcHandle(iface->ze_context, iface->ze_device,
                             key->ipc_handle, 0, &mapped_addr);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("ze_ipc_ep: zeMemOpenIpcHandle failed with error 0x%x "
                  "(context=%p local_device=%p local_dev_id=%d remote_dev_id=%d)",
                  ret, (void*)iface->ze_context, (void*)iface->ze_device,
                  uct_ze_base_get_device_ordinal(iface->ze_device), key->dev_num);
        return UCS_ERR_IO_ERROR;
    }

    ucs_debug("ze_ipc_ep: zeMemOpenIpcHandle succeeded, mapped_addr=%p", mapped_addr);

    /* Calculate offset within the allocation */
    offset          = remote_addr - key->address;
    mapped_rem_addr = (void *)((uintptr_t)mapped_addr + offset);

    /* Allocate event descriptor */
    event_desc = ucs_malloc(sizeof(*event_desc), "uct_ze_ipc_event_desc_t");
    if (event_desc == NULL) {
        ucs_error("failed to allocate event descriptor");
        zeMemCloseIpcHandle(iface->ze_context, mapped_addr);
        return UCS_ERR_NO_MEMORY;
    }

    /* Create event pool and event for tracking completion */
    event_pool_desc.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
    event_pool_desc.count = 1;
    event_pool_desc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;

    ret = zeEventPoolCreate(iface->ze_context, &event_pool_desc, 1,
                            &iface->ze_device, &event_desc->event_pool);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeEventPoolCreate failed with error 0x%x", ret);
        ucs_free(event_desc);
        zeMemCloseIpcHandle(iface->ze_context, mapped_addr);
        return UCS_ERR_IO_ERROR;
    }

    event_desc_ze.stype = ZE_STRUCTURE_TYPE_EVENT_DESC;
    event_desc_ze.index = 0;
    event_desc_ze.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    event_desc_ze.wait   = ZE_EVENT_SCOPE_FLAG_HOST;

    ret = zeEventCreate(event_desc->event_pool, &event_desc_ze,
                        &event_desc->event);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeEventCreate failed with error 0x%x", ret);
        zeEventPoolDestroy(event_desc->event_pool);
        ucs_free(event_desc);
        zeMemCloseIpcHandle(iface->ze_context, mapped_addr);
        return UCS_ERR_IO_ERROR;
    }

    /* Set up source and destination based on direction */
    if (direction == UCT_ZE_IPC_PUT) {
        dst = mapped_rem_addr;
        src = iov[0].buffer;
    } else {
        dst = iov[0].buffer;
        src = mapped_rem_addr;
    }

    /* Append memory copy to command list */
    ret = zeCommandListAppendMemoryCopy(iface->cmd_list, dst, src,
                                        iov[0].length, event_desc->event,
                                        0, NULL);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandListAppendMemoryCopy failed with error 0x%x", ret);
        goto err_cleanup;
    }

    /* Close and execute command list */
    ret = zeCommandListClose(iface->cmd_list);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandListClose failed with error 0x%x", ret);
        goto err_cleanup;
    }

    ret = zeCommandQueueExecuteCommandLists(iface->cmd_queue, 1,
                                            &iface->cmd_list, NULL);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandQueueExecuteCommandLists failed with error 0x%x", ret);
        goto err_cleanup;
    }

    /* Synchronize to ensure command list execution is complete before reset */
    ret = zeCommandQueueSynchronize(iface->cmd_queue, UINT64_MAX);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandQueueSynchronize failed with error 0x%x", ret);
        goto err_cleanup;
    }

    /* Reset command list for next use */
    ret = zeCommandListReset(iface->cmd_list);
    if (ret != ZE_RESULT_SUCCESS) {
        ucs_error("zeCommandListReset failed with error 0x%x", ret);
        goto err_cleanup;
    }

    /* Store event info for progress tracking */
    event_desc->mapped_addr = mapped_addr;
    event_desc->comp        = comp;

    ucs_queue_push(&iface->outstanding, &event_desc->queue);

    ucs_trace("zeCommandListAppendMemoryCopy issued: dst=%p src=%p len=%zu",
              dst, src, iov[0].length);

    return UCS_INPROGRESS;

err_cleanup:
    zeEventDestroy(event_desc->event);
    zeEventPoolDestroy(event_desc->event_pool);
    ucs_free(event_desc);
    zeMemCloseIpcHandle(iface->ze_context, mapped_addr);
    return UCS_ERR_IO_ERROR;
}

UCS_PROFILE_FUNC(ucs_status_t, uct_ze_ipc_ep_get_zcopy,
                 (tl_ep, iov, iovcnt, remote_addr, rkey, comp),
                 uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                 uint64_t remote_addr, uct_rkey_t rkey,
                 uct_completion_t *comp)
{
    ucs_status_t status;

    ucs_info("ze_ipc_ep: GET_ZCOPY called remote_addr=0x%lx iovcnt=%zu total_len=%zu",
             (unsigned long)remote_addr, iovcnt, uct_iov_total_length(iov, iovcnt));

    status = uct_ze_ipc_post_copy(tl_ep, remote_addr, iov, rkey, comp,
                                  UCT_ZE_IPC_GET);
    if (UCS_STATUS_IS_ERR(status)) {
        ucs_error("ze_ipc_ep: GET_ZCOPY failed with status %s", ucs_status_string(status));
        return status;
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_ze_ipc_trace_data(remote_addr, rkey, "GET_ZCOPY [length %zu]",
                          uct_iov_total_length(iov, iovcnt));
    return status;
}


UCS_PROFILE_FUNC(ucs_status_t, uct_ze_ipc_ep_put_zcopy,
                 (tl_ep, iov, iovcnt, remote_addr, rkey, comp),
                 uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                 uint64_t remote_addr, uct_rkey_t rkey,
                 uct_completion_t *comp)
{
    ucs_status_t status;

    ucs_info("ze_ipc_ep: PUT_ZCOPY called remote_addr=0x%lx iovcnt=%zu total_len=%zu",
             (unsigned long)remote_addr, iovcnt, uct_iov_total_length(iov, iovcnt));

    status = uct_ze_ipc_post_copy(tl_ep, remote_addr, iov, rkey, comp,
                                  UCT_ZE_IPC_PUT);
    if (UCS_STATUS_IS_ERR(status)) {
        ucs_error("ze_ipc_ep: PUT_ZCOPY failed with status %s", ucs_status_string(status));
        return status;
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_ze_ipc_trace_data(remote_addr, rkey, "PUT_ZCOPY [length %zu]",
                          uct_iov_total_length(iov, iovcnt));
    return status;
}
