/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2018. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_ZE_IPC_MD_H
#define UCT_ZE_IPC_MD_H

#include <uct/base/uct_md.h>
//#include <uct/ze/base/ze_md.h>
//#include <uct/ze/base/ze_iface.h> //todo: do we need it on XPU?
#include <ucs/datastruct/khash.h>
#include <ucs/type/spinlock.h>
#include <ucs/config/types.h>


typedef enum uct_ze_ipc_key_handle {
    UCT_CUDA_IPC_KEY_HANDLE_TYPE_NO_IPC = 0,
    UCT_CUDA_IPC_KEY_HANDLE_TYPE_LEGACY, /* cudaMalloc memory */
#if HAVE_CUDA_FABRIC
    UCT_CUDA_IPC_KEY_HANDLE_TYPE_VMM, /* cuMemCreate memory */
    UCT_CUDA_IPC_KEY_HANDLE_TYPE_MEMPOOL /* cudaMallocAsync memory */
#endif
} uct_ze_ipc_key_handle_t;


typedef struct uct_ze_ipc_md_handle {
    uct_ze_ipc_key_handle_t handle_type;
    union {
        CUipcMemHandle        legacy;        /* Legacy IPC handle */
#if HAVE_CUDA_FABRIC
        CUmemFabricHandle     fabric_handle; /* VMM/Mallocasync export handle */
#endif
    } handle;
#if HAVE_CUDA_FABRIC
    CUmemPoolPtrExportData    ptr;
    CUmemoryPool              pool;
#endif
    unsigned long long        buffer_id;
} uct_ze_ipc_md_handle_t;

/**
 * @brief ze ipc MD descriptor
 */
typedef struct uct_ze_ipc_md {
    uct_md_t                 super;   /**< Domain info */
    int                      enable_mnnvl; /**< Multi-node NVLINK support status */
} uct_ze_ipc_md_t;


typedef struct uct_ze_ipc_uuid_hash_key {
    int     type;
    CUuuid  uuid;
} uct_ze_ipc_uuid_hash_key_t;


typedef struct {
    /* GPU Device number */
    int     dev_num;
    /* Cache of accessible devices (ucs_ternary_auto_value_t) */
    uint8_t accessible[0];
} uct_ze_ipc_dev_cache_t;


static UCS_F_ALWAYS_INLINE int
uct_ze_ipc_uuid_equals(uct_ze_ipc_uuid_hash_key_t key1,
                         uct_ze_ipc_uuid_hash_key_t key2)
{
    int64_t *a64 = (int64_t *)key1.uuid.bytes;
    int64_t *b64 = (int64_t *)key2.uuid.bytes;

    return (key1.type == key2.type) && (a64[0] == b64[0]) && (a64[1] == b64[1]);
}


static UCS_F_ALWAYS_INLINE khint32_t
uct_ze_ipc_uuid_hash_func(uct_ze_ipc_uuid_hash_key_t key)
{
    int64_t *i64 = (int64_t *)key.uuid.bytes;
    return kh_int64_hash_func(i64[0] ^ i64[1] ^ key.type);
}


KHASH_INIT(ze_ipc_uuid_hash, uct_ze_ipc_uuid_hash_key_t,
           uct_ze_ipc_dev_cache_t*, 1, uct_ze_ipc_uuid_hash_func,
           uct_ze_ipc_uuid_equals);


/**
 * @brief ze ipc component extension
 */
typedef struct {
    uct_component_t             super;
    khash_t(ze_ipc_uuid_hash) uuid_hash;
    pthread_mutex_t             lock;
} uct_ze_ipc_component_t;

extern uct_ze_ipc_component_t uct_ze_ipc_component;

/**
 * @brief ze ipc domain configuration.
 */
typedef struct uct_ze_ipc_md_config {
    uct_md_config_t          super;
    ucs_ternary_auto_value_t enable_mnnvl;
} uct_ze_ipc_md_config_t;


/**
 * @brief list of ze ipc regions registered for memh
 */
typedef struct {
    pid_t           pid;     /* PID as key to resolve peer_map hash */
    int             dev_num; /* GPU Device number */
    ucs_list_link_t list;
} uct_ze_ipc_memh_t;


/**
 * @brief zer ipc region registered for exposure
 */
typedef struct {
    uct_ze_ipc_md_handle_t  ph;     /* Memory handle of GPU memory */
    CUdeviceptr               d_bptr; /* Allocation base address */
    size_t                    b_len;  /* Allocation size */
    ucs_list_link_t           link;
} uct_ze_ipc_lkey_t;


/**
 * @brief ze ipc remote key for put/get
 */
typedef struct {
    uct_ze_ipc_md_handle_t  ph;      /* Memory handle of GPU memory */
    pid_t                     pid;     /* PID as key to resolve peer_map hash */
    CUdeviceptr               d_bptr;  /* Allocation base address */
    size_t                    b_len;   /* Allocation size */
    CUuuid                    uuid;    /* GPU Device UUID */
} uct_ze_ipc_rkey_t;


typedef struct {
    uct_ze_ipc_rkey_t       super;
    int                       stream_id;
} uct_ze_ipc_unpacked_rkey_t;

#endif
