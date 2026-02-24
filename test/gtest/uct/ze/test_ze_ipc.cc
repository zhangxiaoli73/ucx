/**
 * Copyright (c) Intel Corporation, 2025. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <uct/uct_test.h>
#include <uct/api/v2/uct_v2.h>
#include <level_zero/ze_api.h>

extern "C" {
#include <uct/ze/ze_ipc/ze_ipc_md.h>
}

class test_ze_ipc_rma : public uct_test {
protected:
    void init() {
        ze_result_t ret;
        uint32_t driver_count = 0;
        uint32_t device_count = 0;

        uct_test::init();

        /* Initialize Level Zero */
        ret = zeInit(ZE_INIT_FLAG_GPU_ONLY);
        if (ret != ZE_RESULT_SUCCESS) {
            UCS_TEST_SKIP_R("zeInit failed");
        }

        /* Get driver */
        ret = zeDriverGet(&driver_count, NULL);
        if ((ret != ZE_RESULT_SUCCESS) || (driver_count == 0)) {
            UCS_TEST_SKIP_R("No Level Zero drivers found");
        }

        ret = zeDriverGet(&driver_count, &m_ze_driver);
        if (ret != ZE_RESULT_SUCCESS) {
            UCS_TEST_SKIP_R("zeDriverGet failed");
        }

        /* Get device */
        ret = zeDeviceGet(m_ze_driver, &device_count, NULL);
        if ((ret != ZE_RESULT_SUCCESS) || (device_count == 0)) {
            UCS_TEST_SKIP_R("No Level Zero devices found");
        }

        ret = zeDeviceGet(m_ze_driver, &device_count, &m_ze_device);
        if (ret != ZE_RESULT_SUCCESS) {
            UCS_TEST_SKIP_R("zeDeviceGet failed");
        }

        /* Create context */
        ze_context_desc_t context_desc = {};
        ret = zeContextCreate(m_ze_driver, &context_desc, &m_ze_context);
        if (ret != ZE_RESULT_SUCCESS) {
            UCS_TEST_SKIP_R("zeContextCreate failed");
        }

        m_receiver = uct_test::create_entity(0);
        m_entities.push_back(m_receiver);

        m_sender = uct_test::create_entity(0);
        m_entities.push_back(m_sender);

        m_sender->connect(0, *m_receiver, 0);
    }

    void cleanup() {
        if (m_ze_context != NULL) {
            zeContextDestroy(m_ze_context);
        }
        uct_test::cleanup();
    }

    void *alloc_ze_mem(size_t size) {
        ze_device_mem_alloc_desc_t alloc_desc = {};
        void *ptr = NULL;
        ze_result_t ret;

        alloc_desc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
        ret = zeMemAllocDevice(m_ze_context, &alloc_desc, size, 0,
                               m_ze_device, &ptr);
        if (ret != ZE_RESULT_SUCCESS) {
            return NULL;
        }
        return ptr;
    }

    void free_ze_mem(void *ptr) {
        if (ptr != NULL) {
            zeMemFree(m_ze_context, ptr);
        }
    }

    entity *m_sender;
    entity *m_receiver;

    ze_driver_handle_t m_ze_driver;
    ze_device_handle_t m_ze_device;
    ze_context_handle_t m_ze_context;

    static const uint64_t SEED1 = 0xABClu;
    static const uint64_t SEED2 = 0xDEFlu;
};

UCS_TEST_P(test_ze_ipc_rma, basic_query)
{
    uct_iface_attr_t iface_attr;

    ASSERT_UCS_OK(uct_iface_query(m_sender->iface(), &iface_attr));
    EXPECT_TRUE(iface_attr.cap.flags & UCT_IFACE_FLAG_PUT_ZCOPY);
    EXPECT_TRUE(iface_attr.cap.flags & UCT_IFACE_FLAG_GET_ZCOPY);
}

UCS_TEST_P(test_ze_ipc_rma, put_zcopy)
{
    size_t length = 1024;

    mapped_buffer sendbuf(length, SEED1, *m_sender, 0, UCS_MEMORY_TYPE_ZE_DEVICE);
    mapped_buffer recvbuf(length, SEED2, *m_receiver, 0, UCS_MEMORY_TYPE_ZE_DEVICE);

    ASSERT_UCS_OK_OR_INPROGRESS(uct_ep_put_zcopy(m_sender->ep(0),
                                                 sendbuf.iov(), 1,
                                                 (uint64_t)recvbuf.ptr(),
                                                 recvbuf.rkey(), NULL));
    m_sender->flush();
    recvbuf.pattern_check(SEED1);
}

UCS_TEST_P(test_ze_ipc_rma, get_zcopy)
{
    size_t length = 1024;

    mapped_buffer sendbuf(length, SEED1, *m_sender, 0, UCS_MEMORY_TYPE_ZE_DEVICE);
    mapped_buffer recvbuf(length, SEED2, *m_receiver, 0, UCS_MEMORY_TYPE_ZE_DEVICE);

    ASSERT_UCS_OK_OR_INPROGRESS(uct_ep_get_zcopy(m_sender->ep(0),
                                                 sendbuf.iov(), 1,
                                                 (uint64_t)recvbuf.ptr(),
                                                 recvbuf.rkey(), NULL));
    m_sender->flush();
    sendbuf.pattern_check(SEED2);
}

_UCT_INSTANTIATE_TEST_CASE(test_ze_ipc_rma, ze_ipc)

