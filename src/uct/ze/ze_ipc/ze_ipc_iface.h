/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2018. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_ZE_IPC_IFACE_H
#define UCT_ZE_IPC_IFACE_H

#include <uct/base/uct_iface.h>
#include <uct/ze/base/ze_iface.h>
#include <ucs/arch/cpu.h>
#include <ze.h>

#include "ze_ipc_md.h"
#include "ze_ipc_ep.h"
//#include "ze_ipc_cache.h" // todo: need to add a IPC cache?


#define UCT_ZE_IPC_MAX_PEERS 16


typedef struct {
    unsigned                max_poll;            /* query attempts w.o success */
    unsigned                max_streams;         /* # concurrent streams for || progress*/
    unsigned                max_ze_ipc_events; /* max mpool entries */
    int                     enable_cache;        /* enable/disable ipc handle cache */
    ucs_on_off_auto_value_t enable_get_zcopy;    /* enable get_zcopy except for specific platforms */
    double                  bandwidth;           /* estimated bandwidth */
    double                  latency;             /* estimated latency */
    double                  overhead;            /* estimated CPU overhead */
} uct_ze_ipc_iface_config_params_t;


typedef struct {
    uct_ze_iface_t                   super;
    uct_ze_ipc_iface_config_params_t config;
} uct_ze_ipc_iface_t;


typedef struct {
    uct_iface_config_t                 super;
    uct_ze_ipc_iface_config_params_t params;
} uct_ze_ipc_iface_config_t;


typedef struct {
    uct_ze_event_desc_t super;
    void                  *mapped_addr;
    uct_ze_ipc_ep_t     *ep;
    uintptr_t             d_bptr;
    pid_t                 pid;
    CUdevice              ze_device;
} uct_ze_ipc_event_desc_t;


typedef struct {
    uct_ze_ctx_rsc_t    super;
    uct_ze_queue_desc_t queue_desc[UCT_ZE_IPC_MAX_PEERS];
} uct_ze_ipc_ctx_rsc_t;

#endif
