/*
 * Copyright (C) Intel Corporation, 2023-2024. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_ZE_IPC_IFACE_H
#define UCT_ZE_IPC_IFACE_H

#include <uct/base/uct_iface.h>
#include <ucs/arch/cpu.h>
#include <level_zero/ze_api.h>

#include "ze_ipc_md.h"


#define UCT_ZE_IPC_MAX_PEERS 16


typedef struct uct_ze_ipc_iface_config {
    uct_iface_config_t super;
    unsigned           max_poll;         /* query attempts w.o success */
    double             bandwidth;        /* estimated bandwidth */
    double             latency;          /* estimated latency */
    double             overhead;         /* estimated CPU overhead */
} uct_ze_ipc_iface_config_t;


typedef struct uct_ze_ipc_iface {
    uct_base_iface_t             super;
    ze_context_handle_t          ze_context;
    ze_device_handle_t           ze_device;
    ze_command_queue_handle_t    cmd_queue;
    ze_command_list_handle_t     cmd_list;
    uct_ze_ipc_iface_config_t    config;
    ucs_mpool_t                  event_pool;
    ucs_queue_head_t             outstanding;
} uct_ze_ipc_iface_t;


typedef struct uct_ze_ipc_event_desc {
    ze_event_handle_t   event;
    ze_event_pool_handle_t event_pool;
    void               *mapped_addr;
    uct_completion_t   *comp;
    ucs_queue_elem_t    queue;
} uct_ze_ipc_event_desc_t;


#endif
