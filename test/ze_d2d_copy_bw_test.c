/**
 * Level Zero Device-to-Device Copy Bandwidth Test (READ mode)
 *
 * This test copies 512MB from device0 to device1 and measures bandwidth.
 * READ mode: device1's copy engine reads from device0's memory.
 *
 * Compile:
 *   gcc -o ze_d2d_copy_bw_test ze_d2d_copy_bw_test.c -lze_loader
 *
 * Run:
 *   ./ze_d2d_copy_bw_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <level_zero/ze_api.h>

#ifdef _WIN32
#include <windows.h>
static double get_time_sec(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}
#endif

#define MESSAGE_SIZE    (512ULL * 1024 * 1024)  /* 512 MB */
#define NUM_ITERATIONS  10
#define MAX_DEVICES     16
#define MAX_QUEUE_GROUPS 16

#define CHECK_ZE(call, msg) do { \
    ze_result_t ret = (call); \
    if (ret != ZE_RESULT_SUCCESS) { \
        printf("[FAILED] %s: error 0x%x\n", msg, ret); \
        return ret; \
    } \
} while(0)

/* Find the copy engine queue group ordinal */
static int find_copy_queue_ordinal(ze_device_handle_t device, uint32_t *ordinal)
{
    ze_command_queue_group_properties_t queue_props[MAX_QUEUE_GROUPS];
    uint32_t num_queue_groups = MAX_QUEUE_GROUPS;
    uint32_t i;

    /* Initialize structure types */
    for (i = 0; i < MAX_QUEUE_GROUPS; i++) {
        queue_props[i].stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES;
        queue_props[i].pNext = NULL;
    }

    /* Get queue group properties */
    ze_result_t ret = zeDeviceGetCommandQueueGroupProperties(device, &num_queue_groups, queue_props);
    if (ret != ZE_RESULT_SUCCESS) {
        printf("[ERROR] zeDeviceGetCommandQueueGroupProperties failed: 0x%x\n", ret);
        return -1;
    }

    printf("Found %u command queue group(s):\n", num_queue_groups);
    for (i = 0; i < num_queue_groups; i++) {
        printf("  Group[%u]: flags=0x%x, numQueues=%u",
               i, queue_props[i].flags, queue_props[i].numQueues);
        if (queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE)
            printf(" [COMPUTE]");
        if (queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY)
            printf(" [COPY]");
        printf("\n");
    }

    /* Find a queue group that supports COPY but NOT COMPUTE (dedicated copy engine) */
    for (i = 0; i < num_queue_groups; i++) {
        if ((queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) &&
            !(queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE)) {
            *ordinal = i;
            printf("Using dedicated Copy Engine: queue group %u\n", i);
            return 0;
        }
    }

    /* Fallback: find any queue group that supports COPY */
    for (i = 0; i < num_queue_groups; i++) {
        if (queue_props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) {
            *ordinal = i;
            printf("Using Copy-capable queue group %u (may include compute)\n", i);
            return 0;
        }
    }

    printf("[ERROR] No copy-capable queue group found!\n");
    return -1;
}

int main(int argc, char **argv)
{
    ze_driver_handle_t driver;
    ze_device_handle_t devices[MAX_DEVICES];
    ze_context_handle_t context;
    ze_command_queue_handle_t queue;
    ze_command_list_handle_t cmdlist;
    void *dev0_ptr = NULL, *dev1_ptr = NULL;
    void *host_buf = NULL;
    uint32_t num_devices = MAX_DEVICES;
    uint32_t copy_ordinal = 0;
    double start_time, end_time, elapsed, bandwidth;
    int i;

    printf("=== Level Zero Device-to-Device Copy Bandwidth Test (READ) ===\n");
    printf("Message size: %llu MB\n", (unsigned long long)(MESSAGE_SIZE / (1024 * 1024)));
    printf("Iterations: %d\n", NUM_ITERATIONS);
    printf("Mode: READ (device1 reads from device0)\n\n");

    /* Initialize Level Zero */
    CHECK_ZE(zeInit(ZE_INIT_FLAG_GPU_ONLY), "zeInit");

    /* Get driver */
    uint32_t count = 1;
    CHECK_ZE(zeDriverGet(&count, &driver), "zeDriverGet");

    /* Get all devices */
    CHECK_ZE(zeDeviceGet(driver, &num_devices, devices), "zeDeviceGet");
    printf("Found %u device(s)\n", num_devices);

    if (num_devices < 2) {
        printf("[ERROR] Need at least 2 devices for D2D copy test!\n");
        return 1;
    }

    /* Print device info */
    for (i = 0; i < (int)num_devices; i++) {
        ze_device_properties_t props = {ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
        zeDeviceGetProperties(devices[i], &props);
        printf("  Device[%d]: %s\n", i, props.name);
    }
    printf("\n");

    /* Find copy engine queue group on device1 (for READ mode) */
    printf("Querying copy engine on device1 (reader):\n");
    if (find_copy_queue_ordinal(devices[1], &copy_ordinal) != 0) {
        printf("[ERROR] Failed to find copy engine on device1!\n");
        return 1;
    }
    printf("\n");

    /* Create context */
    ze_context_desc_t ctx_desc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
    CHECK_ZE(zeContextCreate(driver, &ctx_desc, &context), "zeContextCreate");

    /* Create command queue on DEVICE1 using COPY ENGINE ordinal (READ mode) */
    ze_command_queue_desc_t queue_desc = {
        .stype    = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
        .ordinal  = copy_ordinal,  /* Use copy engine! */
        .index    = 0,
        .flags    = 0,
        .mode     = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS,
        .priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL,
    };
    CHECK_ZE(zeCommandQueueCreate(context, devices[1], &queue_desc, &queue),
             "zeCommandQueueCreate on device1 (Copy Engine)");

    /* Create command list on DEVICE1 using COPY ENGINE ordinal (READ mode) */
    ze_command_list_desc_t cmdlist_desc = {
        .stype            = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC,
        .commandQueueGroupOrdinal = copy_ordinal,  /* Use copy engine! */
    };
    CHECK_ZE(zeCommandListCreate(context, devices[1], &cmdlist_desc, &cmdlist),
             "zeCommandListCreate on device1 (Copy Engine)");

    /* Allocate device memory on device0 */
    ze_device_mem_alloc_desc_t alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
    CHECK_ZE(zeMemAllocDevice(context, &alloc_desc, MESSAGE_SIZE, 64, devices[0], &dev0_ptr),
             "zeMemAllocDevice on device0");
    printf("Allocated %llu MB on device0: %p\n",
           (unsigned long long)(MESSAGE_SIZE / (1024 * 1024)), dev0_ptr);

    /* Allocate device memory on device1 */
    CHECK_ZE(zeMemAllocDevice(context, &alloc_desc, MESSAGE_SIZE, 64, devices[1], &dev1_ptr),
             "zeMemAllocDevice on device1");
    printf("Allocated %llu MB on device1: %p\n",
           (unsigned long long)(MESSAGE_SIZE / (1024 * 1024)), dev1_ptr);

    /* Initialize source buffer with pattern */
    host_buf = malloc(MESSAGE_SIZE);
    if (!host_buf) {
        printf("[ERROR] Failed to allocate host buffer\n");
        return 1;
    }
    memset(host_buf, 0xAB, MESSAGE_SIZE);

    /* Copy pattern to device0 */
    CHECK_ZE(zeCommandListAppendMemoryCopy(cmdlist, dev0_ptr, host_buf,
                                           MESSAGE_SIZE, NULL, 0, NULL),
             "zeCommandListAppendMemoryCopy (host->dev0)");
    CHECK_ZE(zeCommandListClose(cmdlist), "zeCommandListClose");
    CHECK_ZE(zeCommandQueueExecuteCommandLists(queue, 1, &cmdlist, NULL),
             "zeCommandQueueExecuteCommandLists");
    CHECK_ZE(zeCommandListReset(cmdlist), "zeCommandListReset");
    printf("Initialized device0 buffer with pattern\n\n");

    /* Warm-up run */
    printf("Warming up...\n");
    CHECK_ZE(zeCommandListAppendMemoryCopy(cmdlist, dev1_ptr, dev0_ptr,
                                           MESSAGE_SIZE, NULL, 0, NULL),
             "zeCommandListAppendMemoryCopy (dev0->dev1)");
    CHECK_ZE(zeCommandListClose(cmdlist), "zeCommandListClose");
    CHECK_ZE(zeCommandQueueExecuteCommandLists(queue, 1, &cmdlist, NULL),
             "zeCommandQueueExecuteCommandLists");
    CHECK_ZE(zeCommandListReset(cmdlist), "zeCommandListReset");

    /* Benchmark: Device0 -> Device1 (READ: device1 pulls from device0) */
    printf("Running D2D copy benchmark (device1 reads from device0)...\n");
    start_time = get_time_sec();

    for (i = 0; i < NUM_ITERATIONS; i++) {
        CHECK_ZE(zeCommandListAppendMemoryCopy(cmdlist, dev1_ptr, dev0_ptr,
                                               MESSAGE_SIZE, NULL, 0, NULL),
                 "zeCommandListAppendMemoryCopy");
        CHECK_ZE(zeCommandListClose(cmdlist), "zeCommandListClose");
        CHECK_ZE(zeCommandQueueExecuteCommandLists(queue, 1, &cmdlist, NULL),
                 "zeCommandQueueExecuteCommandLists");
        CHECK_ZE(zeCommandListReset(cmdlist), "zeCommandListReset");
    }

    end_time = get_time_sec();
    elapsed = end_time - start_time;
    bandwidth = (double)(MESSAGE_SIZE * NUM_ITERATIONS) / elapsed / (1024.0 * 1024.0 * 1024.0);

    printf("\n=== Results ===\n");
    printf("Total data transferred: %llu MB\n",
           (unsigned long long)(MESSAGE_SIZE * NUM_ITERATIONS / (1024 * 1024)));
    printf("Total time: %.3f seconds\n", elapsed);
    printf("Average latency per copy: %.3f ms\n", elapsed * 1000.0 / NUM_ITERATIONS);
    printf("Bandwidth: %.2f GB/s\n", bandwidth);

    /* Cleanup */
    free(host_buf);
    zeMemFree(context, dev0_ptr);
    zeMemFree(context, dev1_ptr);
    zeCommandListDestroy(cmdlist);
    zeCommandQueueDestroy(queue);
    zeContextDestroy(context);

    printf("\n[OK] Test completed successfully\n");
    return 0;
}

