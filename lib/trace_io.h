#ifndef TRACE_IO_H
#define TRACE_IO_H

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

struct bin_file_data {
    uint32_t lcore;
    uint64_t tsc_rate;
    uint64_t tsc_timestamp;
    uint32_t obj_idx;
    uint64_t obj_id;
    uint64_t tsc_sc_time; /* object from submit to complete */
    char     tpoint_name[32];       
    uint16_t opc;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cpl;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
};

enum nvme_io_cmd_opc {
    NVME_OPC_FLUSH = 0x00,
    NVME_OPC_WRITE = 0x01,
    NVME_OPC_READ = 0x02,
    NVME_OPC_WRITE_UNCORRECTABLE = 0x04,
    NVME_OPC_COMPARE = 0x05,
    NVME_OPC_WRITE_ZEROES = 0x08,
    NVME_OPC_DATASET_MANAGEMENT = 0x09,
    NVME_OPC_VERIFY = 0x0C,
    NVME_OPC_RESERVATION_REGISTER = 0x0D,
    NVME_OPC_RESERVATION_REPORT = 0x0E,
    NVME_OPC_RESERVATION_ACQUIRE = 0x11,
    NVME_OPC_RESERVATION_RELEASE = 0x15,
    NVME_OPC_COPY = 0x19,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND = 0x79,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV = 0x7A,
    NVME_ZNS_OPC_ZONE_APPEND = 0x7D,
};

enum nvme_zns_mgmt_send_action {
    NVME_ZNS_MGMT_SEND_ACTION_OPEN = 0x01,
    NVME_ZNS_MGMT_SEND_ACTION_CLOSE = 0x02,
    NVME_ZNS_MGMT_SEND_ACTION_FINISH = 0x03,
    NVME_ZNS_MGMT_SEND_ACTION_RESET = 0x04,
    NVME_ZNS_MGMT_SEND_ACTION_OFFLINE = 0x05,
};

enum nvme_zns_mgmt_recv_action {
    NVME_ZNS_MGMT_RECV_ACTION_REPORT_ZONES = 0x00,
    NVME_ZNS_MGMT_RECV_ACTION_EXTENDED_REPORT_ZONES = 0x01,
};

#endif
