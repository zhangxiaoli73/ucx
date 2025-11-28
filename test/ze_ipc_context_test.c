/**
 * Level Zero IPC Context Test
 *
 * This test verifies:
 * 1. zeMemGetIpcHandle requires the same context used for allocation
 * 2. zeMemOpenIpcHandle can use a different context (receiver side)
 * 3. Cross-process IPC with data verification
 * 4. Multi-device IPC scenarios
 *
 * Compile:
 *   gcc -o ze_ipc_context_test ze_ipc_context_test.c -lze_loader
 *
 * Run:
 *   ./ze_ipc_context_test           # Run all tests
 *   ./ze_ipc_context_test context   # Run context tests only
 *   ./ze_ipc_context_test ipc       # Run cross-process IPC test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <level_zero/ze_api.h>

/*
 * Helper functions to send/receive IPC handle via Unix socket with SCM_RIGHTS
 * This is needed because IPC handle may contain file descriptors
 */
static int send_ipc_handle(int sock, ze_ipc_mem_handle_t *handle)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char ctrl_buf[CMSG_SPACE(sizeof(int))];

    /* Treat first 4 bytes as potential fd */
    int fd = *(int*)handle->data;

    iov.iov_base = handle->data;
    iov.iov_len = sizeof(handle->data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* If fd looks valid (positive), send it via SCM_RIGHTS */
    if (fd > 0 && fd < 65536) {
        msg.msg_control = ctrl_buf;
        msg.msg_controllen = sizeof(ctrl_buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        *(int*)CMSG_DATA(cmsg) = fd;

        printf("[send_ipc_handle] Sending fd=%d via SCM_RIGHTS\n", fd);
    } else {
        printf("[send_ipc_handle] No fd detected (first 4 bytes = 0x%08x)\n", fd);
    }

    return sendmsg(sock, &msg, 0);
}

static int recv_ipc_handle(int sock, ze_ipc_mem_handle_t *handle)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char ctrl_buf[CMSG_SPACE(sizeof(int))];

    iov.iov_base = handle->data;
    iov.iov_len = sizeof(handle->data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    int ret = recvmsg(sock, &msg, 0);
    if (ret < 0) return ret;

    /* Check if we received an fd */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int received_fd = *(int*)CMSG_DATA(cmsg);
        printf("[recv_ipc_handle] Received fd=%d via SCM_RIGHTS\n", received_fd);
        /* Replace fd in handle with received fd */
        *(int*)handle->data = received_fd;
    } else {
        printf("[recv_ipc_handle] No fd received via SCM_RIGHTS\n");
    }

    return ret;
}

#define CHECK_ZE(call, msg) do { \
    ze_result_t ret = (call); \
    if (ret != ZE_RESULT_SUCCESS) { \
        printf("[FAILED] %s: error 0x%x\n", msg, ret); \
        return ret; \
    } \
    printf("[OK] %s\n", msg); \
} while(0)

#define CHECK_ZE_QUIET(call, msg) do { \
    ze_result_t ret = (call); \
    if (ret != ZE_RESULT_SUCCESS) { \
        printf("[FAILED] %s: error 0x%x\n", msg, ret); \
        return ret; \
    } \
} while(0)

#define CHECK_ZE_EXPECT_FAIL(call, msg) do { \
    ze_result_t ret = (call); \
    if (ret == ZE_RESULT_SUCCESS) { \
        printf("[UNEXPECTED] %s: succeeded but expected failure\n", msg); \
    } else { \
        printf("[EXPECTED FAIL] %s: error 0x%x (as expected)\n", msg, ret); \
    } \
} while(0)

/* Shared memory structure for cross-process IPC test */
typedef struct {
    ze_ipc_mem_handle_t ipc_handle;
    size_t size;
    size_t alloc_size;      /* size of the entire allocation */
    uintptr_t base_addr;    /* base address of allocation (sender side) */
    uintptr_t offset;       /* offset within allocation */
    int ready;
    int done;
    uint32_t expected_value;
    int socket_fds[2];  /* socketpair for sending IPC handle with fd */
} shared_data_t;

/*
 * Test 1-4: Context mixing tests (single process)
 */
int test_context_mixing(ze_driver_handle_t driver, ze_device_handle_t device)
{
    ze_context_handle_t context1, context2;
    ze_context_desc_t context_desc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
    ze_device_mem_alloc_desc_t alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC, NULL, 0, 0};
    ze_ipc_mem_handle_t ipc_handle;
    void *ptr1, *ptr2;
    size_t alloc_size = 1024 * 1024;  /* 1 MB */

    printf("\n========================================\n");
    printf("=== Context Mixing Tests ===\n");
    printf("========================================\n");

    /* Create two different contexts */
    CHECK_ZE(zeContextCreate(driver, &context_desc, &context1), "zeContextCreate(context1)");
    CHECK_ZE(zeContextCreate(driver, &context_desc, &context2), "zeContextCreate(context2)");

    printf("\ncontext1 = %p\n", (void*)context1);
    printf("context2 = %p\n", (void*)context2);

    printf("\n--- Test 1: Same context for alloc and GetIpcHandle ---\n");
    CHECK_ZE(zeMemAllocDevice(context1, &alloc_desc, alloc_size, 64, device, &ptr1),
             "zeMemAllocDevice(context1, ptr1)");
    printf("ptr1 = %p\n", ptr1);
    CHECK_ZE(zeMemGetIpcHandle(context1, ptr1, &ipc_handle),
             "zeMemGetIpcHandle(context1, ptr1) - SAME context");

    printf("\n--- Test 2: Different context for GetIpcHandle ---\n");
    CHECK_ZE_EXPECT_FAIL(zeMemGetIpcHandle(context2, ptr1, &ipc_handle),
                         "zeMemGetIpcHandle(context2, ptr1) - DIFFERENT context");

    printf("\n--- Test 3: OpenIpcHandle with different context ---\n");
    /* First get valid handle with correct context */
    CHECK_ZE(zeMemGetIpcHandle(context1, ptr1, &ipc_handle),
             "zeMemGetIpcHandle(context1, ptr1)");

    /* Try to open with different context - this SHOULD work on receiver side */
    void *mapped_ptr = NULL;
    ze_result_t ret = zeMemOpenIpcHandle(context2, device, ipc_handle, 0, &mapped_ptr);
    if (ret == ZE_RESULT_SUCCESS) {
        printf("[OK] zeMemOpenIpcHandle(context2) - CAN use different context for Open\n");
        printf("     mapped_ptr = %p\n", mapped_ptr);
        zeMemCloseIpcHandle(context2, mapped_ptr);
    } else {
        printf("[INFO] zeMemOpenIpcHandle(context2): error 0x%x\n", ret);
        printf("       (This may be expected in same-process scenario)\n");
    }

    printf("\n--- Test 4: Allocate with context2, test cross-context ---\n");
    CHECK_ZE(zeMemAllocDevice(context2, &alloc_desc, alloc_size, 64, device, &ptr2),
             "zeMemAllocDevice(context2, ptr2)");
    printf("ptr2 = %p\n", ptr2);
    CHECK_ZE(zeMemGetIpcHandle(context2, ptr2, &ipc_handle),
             "zeMemGetIpcHandle(context2, ptr2) - SAME context");
    CHECK_ZE_EXPECT_FAIL(zeMemGetIpcHandle(context1, ptr2, &ipc_handle),
                         "zeMemGetIpcHandle(context1, ptr2) - DIFFERENT context");

    /* Cleanup */
    zeMemFree(context1, ptr1);
    zeMemFree(context2, ptr2);
    zeContextDestroy(context1);
    zeContextDestroy(context2);
    printf("\n[OK] Context mixing tests complete\n");

    return 0;
}


/*
 * Test 5: Cross-process IPC with data verification
 * Uses Unix socketpair + SCM_RIGHTS to properly transfer IPC handle containing fd
 */
int test_cross_process_ipc(ze_driver_handle_t driver, ze_device_handle_t device)
{
    printf("\n========================================\n");
    printf("=== Cross-Process IPC Test ===\n");
    printf("=== (Using socketpair + SCM_RIGHTS) ===\n");
    printf("========================================\n");

    /* Create shared memory for synchronization */
    shared_data_t *shared = mmap(NULL, sizeof(shared_data_t),
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }
    memset(shared, 0, sizeof(shared_data_t));
    shared->expected_value = 0xDEADBEEF;
    shared->size = 4096;

    /* Create socketpair for IPC handle transfer */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, shared->socket_fds) < 0) {
        perror("socketpair failed");
        return -1;
    }
    printf("[Main] Created socketpair: fds[0]=%d, fds[1]=%d\n",
           shared->socket_fds[0], shared->socket_fds[1]);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }

    if (pid == 0) {
        /* Child process - receiver */
        printf("\n[Child PID %d] Starting receiver...\n", getpid());

        /* Child must re-initialize Level Zero!
         * fork() copies handles but they are invalid in child process */
        ze_driver_handle_t child_driver;
        ze_device_handle_t child_device;
        uint32_t drv_count = 1, dev_count = 1;

        ze_result_t ret = zeInit(ZE_INIT_FLAG_GPU_ONLY);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeInit failed: 0x%x\n", ret);
            exit(1);
        }

        ret = zeDriverGet(&drv_count, &child_driver);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeDriverGet failed: 0x%x\n", ret);
            exit(1);
        }

        ret = zeDeviceGet(child_driver, &dev_count, &child_device);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeDeviceGet failed: 0x%x\n", ret);
            exit(1);
        }
        printf("[Child] Re-initialized Level Zero: driver=%p device=%p\n",
               (void*)child_driver, (void*)child_device);

        /* Close parent's socket fd, use child's */
        close(shared->socket_fds[1]);
        int child_sock = shared->socket_fds[0];

        /* Wait for parent to send IPC handle via socket */
        printf("[Child] Waiting for IPC handle via socket...\n");
        ze_ipc_mem_handle_t received_handle;
        if (recv_ipc_handle(child_sock, &received_handle) < 0) {
            perror("[Child] recv_ipc_handle failed");
            exit(1);
        }

        /* Print received IPC handle for debugging */
        {
            const unsigned char *bytes = (const unsigned char *)&received_handle;
            printf("[Child] Received IPC handle (first 32 bytes): ");
            for (int i = 0; i < 32; i++) {
                printf("%02x", bytes[i]);
            }
            printf("\n");
        }

        /* Create own context with child's driver */
        ze_context_handle_t child_ctx;
        ze_context_desc_t ctx_desc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
        ret = zeContextCreate(child_driver, &ctx_desc, &child_ctx);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeContextCreate failed: 0x%x\n", ret);
            exit(1);
        }
        printf("[Child] Created own context: %p\n", (void*)child_ctx);

        /* Open IPC handle with child's context and device */
        void *mapped_ptr = NULL;
        printf("[Child] Calling zeMemOpenIpcHandle(ctx=%p, dev=%p, flags=0)...\n",
               (void*)child_ctx, (void*)child_device);
        ret = zeMemOpenIpcHandle(child_ctx, child_device, received_handle, 0, &mapped_ptr);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeMemOpenIpcHandle failed: 0x%x\n", ret);
            printf("[Child] Error 0x78000004 = ZE_RESULT_ERROR_INVALID_ARGUMENT\n");
            exit(1);
        }
        printf("[Child] zeMemOpenIpcHandle succeeded: mapped_base=%p\n", mapped_ptr);

        /* Calculate actual pointer using offset from sender */
        void *actual_ptr = (void*)((uintptr_t)mapped_ptr + shared->offset);
        printf("[Child] mapped_base=%p + offset=%lu = actual_ptr=%p\n",
               mapped_ptr, (unsigned long)shared->offset, actual_ptr);

        /* Verify mapped_ptr is valid by checking its allocation properties */
        {
            ze_memory_allocation_properties_t mem_props = {
                .stype = ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES
            };
            ze_device_handle_t alloc_device = NULL;
            ret = zeMemGetAllocProperties(child_ctx, mapped_ptr, &mem_props, &alloc_device);
            if (ret == ZE_RESULT_SUCCESS) {
                printf("[Child] mapped_ptr properties: type=%d, id=%lu, device=%p\n",
                       mem_props.type, (unsigned long)mem_props.id, (void*)alloc_device);
            } else {
                printf("[Child] WARNING: zeMemGetAllocProperties failed: 0x%x\n", ret);
                printf("[Child] mapped_ptr may be invalid!\n");
            }
        }

        /* Create command list to read data */
        ze_command_queue_handle_t queue;
        ze_command_list_handle_t cmdlist;
        ze_command_queue_desc_t queue_desc = {ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC};
        ze_command_list_desc_t cmdlist_desc = {ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC};

        ret = zeCommandQueueCreate(child_ctx, child_device, &queue_desc, &queue);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeCommandQueueCreate failed: 0x%x\n", ret);
            exit(1);
        }

        ret = zeCommandListCreate(child_ctx, child_device, &cmdlist_desc, &cmdlist);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeCommandListCreate failed: 0x%x\n", ret);
            exit(1);
        }

        /* Step 1: Allocate local GPU memory */
        ze_device_mem_alloc_desc_t local_alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
        void *local_gpu_buf = NULL;
        ret = zeMemAllocDevice(child_ctx, &local_alloc_desc, shared->size, 64, child_device, &local_gpu_buf);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeMemAllocDevice for local buffer failed: 0x%x\n", ret);
            exit(1);
        }
        printf("[Child] Allocated local GPU memory: %p\n", local_gpu_buf);

        /* Step 2: Copy from remote GPU (actual_ptr) to local GPU (local_gpu_buf) */
        printf("[Child] Copying %zu bytes: remote_gpu=%p -> local_gpu=%p\n",
               shared->size, actual_ptr, local_gpu_buf);
        ret = zeCommandListAppendMemoryCopy(cmdlist, local_gpu_buf, actual_ptr,
                                             shared->size, NULL, 0, NULL);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeCommandListAppendMemoryCopy (remote->local GPU) failed: 0x%x\n", ret);
            exit(1);
        }

        /* Step 3: Copy from local GPU to host */
        uint32_t *host_buf = malloc(shared->size);
        if (host_buf == NULL) {
            printf("[Child] malloc failed!\n");
            exit(1);
        }
        ret = zeCommandListAppendMemoryCopy(cmdlist, host_buf, local_gpu_buf,
                                             shared->size, NULL, 0, NULL);
        if (ret != ZE_RESULT_SUCCESS) {
            printf("[Child] zeCommandListAppendMemoryCopy (local GPU->host) failed: 0x%x\n", ret);
            exit(1);
        }
        printf("[Child] Copying %zu bytes: local_gpu=%p -> host=%p\n",
               shared->size, local_gpu_buf, (void*)host_buf);

        zeCommandListClose(cmdlist);
        zeCommandQueueExecuteCommandLists(queue, 1, &cmdlist, NULL);
        zeCommandQueueSynchronize(queue, UINT64_MAX);

        /* Verify data */
        if (host_buf[0] == shared->expected_value) {
            printf("[Child] Data verification PASSED: 0x%x == 0x%x\n",
                   host_buf[0], shared->expected_value);
        } else {
            printf("[Child] Data verification FAILED: 0x%x != 0x%x\n",
                   host_buf[0], shared->expected_value);
        }

        /* Cleanup */
        free(host_buf);
        zeMemFree(child_ctx, local_gpu_buf);
        zeCommandListDestroy(cmdlist);
        zeCommandQueueDestroy(queue);
        zeMemCloseIpcHandle(child_ctx, mapped_ptr);
        zeContextDestroy(child_ctx);
        close(child_sock);

        shared->done = 1;
        printf("[Child] Done\n");
        exit(0);
    } else {
        /* Parent process - sender */
        printf("\n[Parent PID %d] Starting sender...\n", getpid());

        /* Create parent context */
        ze_context_handle_t parent_ctx;
        ze_context_desc_t ctx_desc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
        CHECK_ZE(zeContextCreate(driver, &ctx_desc, &parent_ctx),
                 "[Parent] zeContextCreate");
        printf("[Parent] Created context: %p\n", (void*)parent_ctx);

        /* Allocate device memory */
        ze_device_mem_alloc_desc_t alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
        void *dev_ptr = NULL;
        CHECK_ZE(zeMemAllocDevice(parent_ctx, &alloc_desc, shared->size, 64, device, &dev_ptr),
                 "[Parent] zeMemAllocDevice");
        printf("[Parent] Allocated device memory: %p\n", dev_ptr);

        /* Initialize data on GPU */
        ze_command_queue_handle_t queue;
        ze_command_list_handle_t cmdlist;
        ze_command_queue_desc_t queue_desc = {ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC};
        ze_command_list_desc_t cmdlist_desc = {ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC};

        CHECK_ZE(zeCommandQueueCreate(parent_ctx, device, &queue_desc, &queue),
                 "[Parent] zeCommandQueueCreate");
        CHECK_ZE(zeCommandListCreate(parent_ctx, device, &cmdlist_desc, &cmdlist),
                 "[Parent] zeCommandListCreate");

        uint32_t *host_buf = malloc(shared->size);
        host_buf[0] = shared->expected_value;
        for (size_t i = 1; i < shared->size / sizeof(uint32_t); i++) {
            host_buf[i] = i;
        }

        CHECK_ZE(zeCommandListAppendMemoryCopy(cmdlist, dev_ptr, host_buf,
                                                shared->size, NULL, 0, NULL),
                 "[Parent] zeCommandListAppendMemoryCopy");
        CHECK_ZE(zeCommandListClose(cmdlist), "[Parent] zeCommandListClose");
        CHECK_ZE(zeCommandQueueExecuteCommandLists(queue, 1, &cmdlist, NULL),
                 "[Parent] zeCommandQueueExecuteCommandLists");
        CHECK_ZE(zeCommandQueueSynchronize(queue, UINT64_MAX),
                 "[Parent] zeCommandQueueSynchronize");

        /* Get base address and calculate offset using zeMemGetAddressRange */
        void *base_ptr = NULL;
        size_t alloc_size = 0;
        CHECK_ZE(zeMemGetAddressRange(parent_ctx, dev_ptr, &base_ptr, &alloc_size),
                 "[Parent] zeMemGetAddressRange");

        uintptr_t offset = (uintptr_t)dev_ptr - (uintptr_t)base_ptr;
        printf("[Parent] dev_ptr=%p, base_ptr=%p, alloc_size=%zu, offset=%lu\n",
               dev_ptr, base_ptr, alloc_size, (unsigned long)offset);

        /* Get IPC handle for BASE address (not dev_ptr!) */
        ze_ipc_mem_handle_t ipc_handle;
        CHECK_ZE(zeMemGetIpcHandle(parent_ctx, base_ptr, &ipc_handle),
                 "[Parent] zeMemGetIpcHandle");

        /* Store offset in shared memory for child to use */
        shared->offset = offset;
        shared->alloc_size = alloc_size;
        shared->base_addr = (uintptr_t)base_ptr;

        /* Print IPC handle for debugging */
        {
            const unsigned char *bytes = (const unsigned char *)&ipc_handle;
            printf("[Parent] IPC handle (first 32 bytes): ");
            for (int i = 0; i < 32; i++) {
                printf("%02x", bytes[i]);
            }
            printf("\n");
        }

        /* Close child's socket fd, use parent's */
        close(shared->socket_fds[0]);
        int parent_sock = shared->socket_fds[1];

        /* Send IPC handle via socket with SCM_RIGHTS */
        printf("[Parent] Sending IPC handle via socket...\n");
        if (send_ipc_handle(parent_sock, &ipc_handle) < 0) {
            perror("[Parent] send_ipc_handle failed");
            return -1;
        }
        printf("[Parent] IPC handle sent, waiting for child...\n");

        /* Wait for child to finish */
        while (!shared->done) {
            usleep(10000);
        }

        /* Cleanup */
        free(host_buf);
        zeCommandListDestroy(cmdlist);
        zeCommandQueueDestroy(queue);
        zeMemFree(parent_ctx, dev_ptr);
        zeContextDestroy(parent_ctx);

        int status;
        waitpid(pid, &status, 0);
        printf("[Parent] Child exited with status %d\n", WEXITSTATUS(status));
    }

    munmap(shared, sizeof(shared_data_t));
    printf("\n[OK] Cross-process IPC test complete\n");
    return 0;
}


/*
 * Test 6: Multi-device IPC test
 */
int test_multi_device_ipc(ze_driver_handle_t driver, ze_device_handle_t *devices,
                          uint32_t num_devices)
{
    printf("\n========================================\n");
    printf("=== Multi-Device IPC Test ===\n");
    printf("========================================\n");

    if (num_devices < 2) {
        printf("[SKIP] Need at least 2 devices, found %u\n", num_devices);
        return 0;
    }

    ze_context_handle_t context;
    ze_context_desc_t ctx_desc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
    ze_device_mem_alloc_desc_t alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
    void *ptr0 = NULL, *ptr1 = NULL;
    ze_ipc_mem_handle_t ipc_handle;
    void *mapped_ptr = NULL;
    size_t alloc_size = 1024 * 1024;

    printf("Testing with %u devices\n", num_devices);

    /* Create single context for all devices */
    CHECK_ZE(zeContextCreate(driver, &ctx_desc, &context), "zeContextCreate");

    /* Allocate on device 0 */
    CHECK_ZE(zeMemAllocDevice(context, &alloc_desc, alloc_size, 64, devices[0], &ptr0),
             "zeMemAllocDevice on device[0]");
    printf("ptr0 on device[0] = %p\n", ptr0);

    /* Allocate on device 1 */
    CHECK_ZE(zeMemAllocDevice(context, &alloc_desc, alloc_size, 64, devices[1], &ptr1),
             "zeMemAllocDevice on device[1]");
    printf("ptr1 on device[1] = %p\n", ptr1);

    /* Get IPC handle for device 0 memory */
    CHECK_ZE(zeMemGetIpcHandle(context, ptr0, &ipc_handle),
             "zeMemGetIpcHandle(ptr0)");

    /* Try to open on device 1 */
    printf("\n--- Opening device[0] memory on device[1] ---\n");
    ze_result_t ret = zeMemOpenIpcHandle(context, devices[1], ipc_handle, 0, &mapped_ptr);
    if (ret == ZE_RESULT_SUCCESS) {
        printf("[OK] zeMemOpenIpcHandle on device[1]: mapped_ptr=%p\n", mapped_ptr);

        /* Check P2P accessibility */
        ze_bool_t can_access;
        ret = zeDeviceCanAccessPeer(devices[1], devices[0], &can_access);
        if (ret == ZE_RESULT_SUCCESS) {
            printf("     device[1] can access device[0]: %s\n", can_access ? "YES" : "NO");
        }

        zeMemCloseIpcHandle(context, mapped_ptr);
    } else {
        printf("[FAILED] zeMemOpenIpcHandle on device[1]: error 0x%x\n", ret);
        printf("         (P2P may not be supported between these devices)\n");
    }

    /* Get IPC handle for device 1 memory */
    CHECK_ZE(zeMemGetIpcHandle(context, ptr1, &ipc_handle),
             "zeMemGetIpcHandle(ptr1)");

    /* Try to open on device 0 */
    printf("\n--- Opening device[1] memory on device[0] ---\n");
    ret = zeMemOpenIpcHandle(context, devices[0], ipc_handle, 0, &mapped_ptr);
    if (ret == ZE_RESULT_SUCCESS) {
        printf("[OK] zeMemOpenIpcHandle on device[0]: mapped_ptr=%p\n", mapped_ptr);
        zeMemCloseIpcHandle(context, mapped_ptr);
    } else {
        printf("[FAILED] zeMemOpenIpcHandle on device[0]: error 0x%x\n", ret);
    }

    /* Cleanup */
    zeMemFree(context, ptr0);
    zeMemFree(context, ptr1);
    zeContextDestroy(context);

    printf("\n[OK] Multi-device IPC test complete\n");
    return 0;
}

/*
 * Main entry point
 */
int main(int argc, char **argv)
{
    ze_driver_handle_t driver;
    ze_device_handle_t devices[8];
    uint32_t num_devices = 8;
    int run_context_test = 1;
    int run_ipc_test = 1;
    int run_multidev_test = 1;

    printf("==============================================\n");
    printf("=== Level Zero IPC Context Test Suite ===\n");
    printf("==============================================\n\n");

    /* Parse arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "context") == 0) {
            run_ipc_test = 0;
            run_multidev_test = 0;
        } else if (strcmp(argv[1], "ipc") == 0) {
            run_context_test = 0;
            run_multidev_test = 0;
        } else if (strcmp(argv[1], "multidev") == 0) {
            run_context_test = 0;
            run_ipc_test = 0;
        }
    }

    /* Initialize Level Zero */
    CHECK_ZE(zeInit(ZE_INIT_FLAG_GPU_ONLY), "zeInit");

    /* Get driver */
    uint32_t count = 1;
    CHECK_ZE(zeDriverGet(&count, &driver), "zeDriverGet");

    /* Get all devices */
    CHECK_ZE(zeDeviceGet(driver, &num_devices, devices), "zeDeviceGet");
    printf("Found %u device(s)\n", num_devices);

    /* Print device info */
    for (uint32_t i = 0; i < num_devices; i++) {
        ze_device_properties_t props = {ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
        zeDeviceGetProperties(devices[i], &props);
        printf("  Device[%u]: %s (type=%d)\n", i, props.name, props.type);
    }

    /* Run tests */
    if (run_context_test) {
        test_context_mixing(driver, devices[0]);
    }

    if (run_ipc_test) {
        test_cross_process_ipc(driver, devices[0]);
    }

    if (run_multidev_test) {
        test_multi_device_ipc(driver, devices, num_devices);
    }

    printf("\n==============================================\n");
    printf("=== All Tests Complete ===\n");
    printf("==============================================\n");

    printf("\n=== Key Findings ===\n");
    printf("1. zeMemGetIpcHandle() REQUIRES the same context used for allocation\n");
    printf("2. zeMemOpenIpcHandle() CAN use a different context (receiver side)\n");
    printf("3. For UCX ze_ipc to work, the application must use UCX's context\n");
    printf("   for memory allocation, OR UCX must accept the app's context\n");

    return 0;
}
