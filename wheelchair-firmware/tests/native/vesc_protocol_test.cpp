// VESC frame decoding, freshness and RPM scaling; no CAN driver needed.
#include "vesc_protocol.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static void be32(uint8_t *p, int32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

int main()
{
    // Wheel RPM keeps its fraction on the way to ERPM. The old helper took an
    // int, so 0.9 RPM became 0 and every command lost up to one wheel RPM.
    assert(vescWheelRpmToErpm(0.9f) == 52);
    assert(vescWheelRpmToErpm(-0.9f) == -52);
    assert(vescWheelRpmToErpm(10.0f) == 582);
    assert(vescWheelRpmToErpm(0.0f) == 0);

    VescStatus left{}, right{};
    uint8_t data[8]{};
    be32(data, 5818); // 100 wheel RPM
    data[4] = 0; data[5] = 123; // 12.3 A
    data[6] = 0x01; data[7] = 0xF4; // duty 0.5
    const uint32_t statusLeft = 2 | (uint32_t{VESC_STATUS} << 8);
    assert(!vescFresh(left, 1000, 200)); // never heard from
    vescDecode(statusLeft, true, 8, data, 2, left, 1000);
    vescDecode(statusLeft, true, 8, data, 26, right, 1000); // other controller: ignored
    assert(left.erpm == 5818 && std::fabs(vescWheelRpm(left) - 100.0f) < 0.01f);
    assert(std::fabs(left.current - 12.3f) < 0.001f && std::fabs(left.duty - 0.5f) < 0.001f);
    assert(left.rpmFrames == 1 && right.rpmFrames == 0 && right.frames == 0);

    // Negative ERPM, and frames that must not count as a speed report.
    be32(data, -2909);
    vescDecode(statusLeft, true, 8, data, 2, left, 1010);
    assert(std::fabs(vescWheelRpm(left) + 50.0f) < 0.01f);
    vescDecode(statusLeft, false, 8, data, 2, left, 1020); // standard id
    vescDecode(statusLeft, true, 7, data, 2, left, 1020);  // short
    assert(left.rpmFrames == 2 && left.lastRpmMs == 1010);

    // Fresh up to and including the timeout, stale after.
    assert(vescFresh(left, 1210, 200));
    assert(!vescFresh(left, 1211, 200));
    // Temperature and voltage frames keep arriving but do not make the speed fresh.
    vescDecode(2 | (uint32_t{VESC_STATUS_4} << 8), true, 8, data, 2, left, 1300);
    vescDecode(2 | (uint32_t{VESC_STATUS_5} << 8), true, 8, data, 2, left, 1300);
    assert(left.frames == 4 && left.lastMs == 1300);
    assert(!vescFresh(left, 1300, 200));

    // Survives millis() rollover.
    VescStatus late{};
    vescDecode(statusLeft, true, 8, data, 2, late, UINT32_MAX - 50);
    assert(vescFresh(late, 100, 200));
    assert(!vescFresh(late, 151, 200));
    puts("VESC protocol tests passed");
}
