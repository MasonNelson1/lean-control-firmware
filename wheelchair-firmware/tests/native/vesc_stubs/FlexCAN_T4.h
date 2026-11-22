#pragma once
// Host stand-in for FlexCAN_T4: records written frames; nothing is received.
#include "Arduino.h"
#include <vector>

typedef struct CAN_message_t
{
    uint32_t id = 0;
    uint16_t timestamp = 0;
    uint8_t idhit = 0;
    struct
    {
        bool extended = 0;
        bool remote = 0;
        bool overrun = 0;
        bool reserved = 0;
    } flags;
    uint8_t len = 8;
    uint8_t buf[8] = {0};
    int8_t mb = 0;
    uint8_t bus = 0;
    bool seq = 0;
} CAN_message_t;

enum CAN_DEV_TABLE { CAN1 };
enum FLEXCAN_RXQUEUE_TABLE { RX_SIZE_256 = 256 };
enum FLEXCAN_TXQUEUE_TABLE { TX_SIZE_16 = 16 };
enum FLEXCAN_FILTER { ACCEPT_ALL };

template <CAN_DEV_TABLE B, FLEXCAN_RXQUEUE_TABLE R, FLEXCAN_TXQUEUE_TABLE T>
struct FlexCAN_T4
{
    std::vector<CAN_message_t> sent;
    bool full = false; // true: write() refuses, like a full TX queue
    void begin() {}
    void setBaudRate(uint32_t) {}
    void setMaxMB(int) {}
    void enableFIFO() {}
    void setFIFOFilter(FLEXCAN_FILTER) {}
    int read(CAN_message_t &) { return 0; }
    int write(const CAN_message_t &m)
    {
        if (full)
            return 0;
        sent.push_back(m);
        return 1;
    }
};
