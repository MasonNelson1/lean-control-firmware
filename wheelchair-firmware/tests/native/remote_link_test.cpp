// Host test (does not command hardware):
// g++ -std=c++11 -Wall -Wextra -Werror -I src tests/native/remote_link_test.cpp -o /tmp/remote_link_test
// /tmp/remote_link_test
#include "remote_link.h"
#include <cassert>
#include <cstdio>

static void drive(RemoteLink &r, int16_t a, int16_t b, uint32_t t) {
  const uint8_t p[5] = {RemoteLink::PACKET_DRIVE, (uint8_t)a, (uint8_t)((uint16_t)a >> 8),
                        (uint8_t)b, (uint8_t)((uint16_t)b >> 8)};
  r.onPacket(p, 5, t);
}
static void one(RemoteLink &r, uint8_t type, uint32_t t) { r.onPacket(&type, 1, t); }

int main() {
  {  // Zero until the first packet, then follows the controller immediately.
    RemoteLink r(250);
    r.service(5000);
    assert(!r.driveAllowed() && r.left() == 0.0f);
    drive(r, 32767, -32767, 5001);
    r.service(5001);
    assert(r.left() == 1.0f && r.right() == -1.0f);
  }
  {  // Little-endian decode.
    RemoteLink r(250);
    const uint8_t p[5] = {1, 0xFF, 0x3F, 0x01, 0xC0};  // +16383, -16383
    r.onPacket(p, 5, 1);
    assert(r.left() > .49f && r.left() < .51f && r.right() < -.49f && r.right() > -.51f);
  }
  {  // Silence: zero just past the timeout, resumes when packets return.
    RemoteLink r(250);
    drive(r, 16000, 16000, 20);
    r.service(270); assert(r.driveAllowed());
    r.service(271); assert(!r.driveAllowed() && r.left() == 0.0f && r.linkTimeouts() == 1);
    r.service(5000); assert(r.linkTimeouts() == 1);
    drive(r, 8000, 8000, 5001); r.service(5001);
    assert(r.driveAllowed() && r.left() > .24f);
  }
  {  // Soft stop is immediate and holds, including against drive packets.
    RemoteLink r(250);
    drive(r, 16000, 16000, 1);
    one(r, RemoteLink::PACKET_SOFT_STOP, 2);
    assert(r.softStopped() && r.left() == 0.0f);
    drive(r, 16000, 16000, 3); r.service(3);
    assert(r.left() == 0.0f);
    for (uint32_t t = 10; t < 2000; t += 10) {  // controller repeats at 100 Hz
      one(r, RemoteLink::PACKET_SOFT_STOP, t);
      r.service(t);
      assert(r.softStopped());
    }
  }
  {  // Clear: zero until the next drive packet, no stale command.
    RemoteLink r(250);
    drive(r, 16000, 16000, 1);
    one(r, RemoteLink::PACKET_SOFT_STOP, 2);
    one(r, RemoteLink::PACKET_SOFT_STOP_CLEAR, 3);
    assert(!r.softStopped() && r.left() == 0.0f);
    drive(r, 8000, 8000, 4);
    assert(r.left() > .24f);
  }
  {  // A dropped link releases the stop, then the reconnected controller is obeyed.
    RemoteLink r(250);
    one(r, RemoteLink::PACKET_SOFT_STOP, 100);
    r.service(350); assert(r.softStopped());
    r.service(351); assert(!r.softStopped() && r.left() == 0.0f);
    drive(r, 16000, 16000, 9000);
    assert(r.left() > .48f);
  }
  {  // Bad packets are rejected and refresh nothing.
    RemoteLink r(250);
    drive(r, 16000, 16000, 2);
    const uint8_t shortDrive[3] = {1, 0, 0}; r.onPacket(shortDrive, 3, 100);
    const uint8_t longStop[2] = {2, 0}; r.onPacket(longStop, 2, 100);
    const uint8_t longClear[2] = {3, 0}; r.onPacket(longClear, 2, 100);
    drive(r, INT16_MIN, 0, 100); drive(r, 0, INT16_MIN, 100);
    one(r, 0x7E, 100); r.onPacket(nullptr, 0, 100);
    assert(r.rejected() == 7 && r.left() > .48f);
    r.service(252); assert(r.driveAllowed());
    r.service(253); assert(!r.driveAllowed());
  }
  {  // millis() rollover.
    RemoteLink r(250);
    const uint32_t t = 0xFFFFFFF0u;
    drive(r, 16000, 16000, t);
    r.service(t + 100); assert(r.driveAllowed());
    r.service(t + 251); assert(!r.driveAllowed());
  }
  {  // Largest gap between drive packets.
    RemoteLink r(250);
    drive(r, 0, 0, 200); drive(r, 0, 0, 230);
    assert(r.maxDriveGapMs() == 30);
  }
  puts("remote_link_test: PASS");
}
