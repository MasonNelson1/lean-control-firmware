// Host test for the VESC CAN telemetry path (no hardware): status-frame
// handling, long-reply reassembly with real config bytes, the one-reply-at-a-
// time GET_VALUES poll, fault lines, and the DBG VESC / VVAL formats.
#include "vesc_monitor.h"
#include "vesc_real_conf.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

TestSerial Serial, Serial1;

namespace
{
// Splits a reply the way VESC's comm_can_send_buffer (comm_can.c, 6.06) sends
// it to PROBE_CAN_ID: FILL_RX_BUFFER (7 bytes, offsets < 256), then
// FILL_RX_BUFFER_LONG (6 bytes), then PROCESS_RX_BUFFER with length and CRC.
std::vector<CAN_message_t> vescSendBuffer(uint8_t fromId, const uint8_t *data, unsigned len)
{
  std::vector<CAN_message_t> frames;
  auto frame = [&](uint8_t type, std::initializer_list<uint8_t> head, const uint8_t *body, unsigned n) {
    CAN_message_t m;
    m.flags.extended = 1;
    m.id = vescprobe::PROBE_CAN_ID | ((uint32_t)type << 8);
    m.len = 0;
    for (uint8_t h : head)
      m.buf[m.len++] = h;
    for (unsigned i = 0; i < n; ++i)
      m.buf[m.len++] = body[i];
    frames.push_back(m);
  };
  unsigned end_a = 0;
  for (unsigned i = 0; i < len; i += 7)
  {
    if (i > 255)
      break;
    end_a = i + 7;
    frame(vescprobe::CAN_FILL_RX_BUFFER, {(uint8_t)i}, data + i, i + 7 <= len ? 7 : len - i);
  }
  for (unsigned i = end_a; i < len; i += 6)
    frame(vescprobe::CAN_FILL_RX_BUFFER_LONG, {(uint8_t)(i >> 8), (uint8_t)(i & 0xFF)}, data + i,
          i + 6 <= len ? 6 : len - i);
  const uint16_t crc = vescprobe::crc16(data, len);
  frame(vescprobe::CAN_PROCESS_RX_BUFFER,
        {fromId, 1, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF), (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)}, nullptr, 0);
  return frames;
}

bool feed(const std::vector<CAN_message_t> &frames, vescprobe::Reply &r)
{
  bool done = false;
  for (const auto &f : frames)
    done = vescprobe::handleFrame(f, r) || done;
  return done;
}

// A GET_VALUES reply as commands.c (6.06) builds it; see vesc_decode_test.cpp.
std::vector<uint8_t> valuesReply(uint8_t fault, float motorAmps)
{
  std::vector<uint8_t> b;
  auto i16 = [&](int16_t v) { b.push_back((uint16_t)v >> 8); b.push_back(v & 0xFF); };
  auto i32 = [&](int32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(((uint32_t)v >> s) & 0xFF); };
  b.push_back(VESC_COMM_GET_VALUES);
  i16(254); i16(-2700);                                 // temps ×10
  i32((int32_t)(motorAmps * 100)); i32(-321); i32(12); i32((int32_t)(motorAmps * 100)); // currents ×100
  i16(-456); i32(-8123); i16(523);                      // duty ×1000, erpm, vin ×10
  i32(1234); i32(567); i32(64321); i32(25000);          // Ah, Ah charged, Wh, Wh charged ×1e4
  i32(-123456); i32(654321);                            // tachometer, abs
  b.push_back(fault);
  i32(0); b.push_back(2); i16(301); i16(302); i16(303); i32(1234); i32(-20500); b.push_back(0);
  return b;
}

vescprobe::Reply reply(uint8_t fromId, const std::vector<uint8_t> &data, bool crcOk = true)
{
  vescprobe::Reply r;
  r.done = true;
  r.fromId = fromId;
  r.len = data.size();
  r.crcOk = crcOk;
  memcpy(r.data, data.data(), data.size());
  return r;
}

size_t count(const std::string &text, const std::string &what)
{
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
    ++n;
  return n;
}

std::string lastLine(const std::string &text, const std::string &prefix)
{
  const size_t at = text.rfind(prefix);
  return at == std::string::npos ? "" : text.substr(at, text.find('\n', at) - at);
}
} // namespace

int main()
{
  // ---- vescHandleFrame: id filter, extended only, all five status types.
  {
    VescStatus left{}, right{};
    CAN_message_t m;
    m.flags.extended = 1;
    m.len = 8;
    const uint8_t s4[8] = {0x01, 0x60, 0xF5, 0x74, 0xFF, 0xD3, 0x00, 0x00}; // 35.2 °C, -270.0 °C, -4.5 A
    memcpy(m.buf, s4, 8);
    m.id = 26 | (VESC_STATUS_4 << 8);
    fakeMillis = 500;
    vescHandleFrame(m, 2, left);
    vescHandleFrame(m, 26, right);
    assert(left.frames == 0 && right.frames == 1 && right.typeFrames[3] == 1);
    assert(std::fabs(right.mosfetTempC - 35.2f) < 1e-4f && std::fabs(right.motorTempC + 270.0f) < 1e-4f);
    assert(std::fabs(right.inputCurrent + 4.5f) < 1e-4f);
    m.flags.extended = 0;
    vescHandleFrame(m, 26, right);
    assert(right.frames == 1);
  }

  // ---- Real config bytes from the chair survive the CAN long-buffer path.
  {
    vescprobe::Reply r;
    assert(feed(vescSendBuffer(2, REAL_MCCONF_2, sizeof REAL_MCCONF_2), r));
    assert(r.crcOk && r.fromId == 2 && r.len == sizeof REAL_MCCONF_2 && r.len == 484);
    assert(memcmp(r.data, REAL_MCCONF_2, r.len) == 0);
    assert(r.data[0] == vescprobe::COMM_GET_MCCONF && vescBe32(r.data + 1) == 788332866); // MCCONF_SIGNATURE
    vescprobe::Reply a;
    assert(feed(vescSendBuffer(26, REAL_APPCONF_26, sizeof REAL_APPCONF_26), a));
    assert(a.crcOk && a.len == 279 && a.data[0] == vescprobe::COMM_GET_APPCONF && vescBe32(a.data + 1) == 2099347128);

    // Two VESCs answering at once mix their FILL frames (they carry no sender
    // id); the CRC rejects the result. This is why the monitor asks one at a time.
    auto first = vescSendBuffer(2, REAL_MCCONF_2, sizeof REAL_MCCONF_2);
    auto second = vescSendBuffer(26, REAL_MCCONF_26, sizeof REAL_MCCONF_26);
    std::vector<CAN_message_t> mixed;
    for (size_t i = 0; i + 1 < first.size(); ++i)
    {
      mixed.push_back(first[i]);
      mixed.push_back(second[i]);
    }
    mixed.push_back(first.back());
    vescprobe::Reply bad;
    assert(feed(mixed, bad) && !bad.crcOk);
  }

  // ---- Poll schedule: alternate, 100 ms apart, one reply in flight at a time.
  {
    VescBus bus;
    vescmon::Monitor mon;
    fakeMillis = 1000;
    mon.update(bus, fakeMillis, 2, 26, true);
    assert(bus.sent.size() == 1);
    const CAN_message_t &ask = bus.sent[0];
    assert(ask.flags.extended && ask.id == (2u | (vescprobe::CAN_PROCESS_SHORT_BUFFER << 8)));
    assert(ask.len == 3 && ask.buf[0] == vescprobe::PROBE_CAN_ID && ask.buf[1] == 0 && ask.buf[2] == VESC_COMM_GET_VALUES);
    assert(mon.busy(1000));
    // No reply yet: nothing more is asked even when the poll period passes.
    for (fakeMillis = 1001; fakeMillis < 1150; fakeMillis += 7)
      mon.update(bus, fakeMillis, 2, 26, true);
    assert(bus.sent.size() == 1);
    // The reply frees the bus; the next poll goes to the other VESC.
    assert(mon.onReply(reply(2, valuesReply(0, 1.5f)), 1012));
    assert(!mon.busy(1012) && mon.side(0).replies == 1 && mon.side(0).values.fault == 0);
    fakeMillis = 1160;
    mon.update(bus, fakeMillis, 2, 26, true);
    assert(bus.sent.size() == 2 && (bus.sent[1].id & 0xFF) == 26);
    // No answer within REPLY_WAIT_MS: counted lost, and polling resumes.
    fakeMillis = 1160 + vescmon::REPLY_WAIT_MS;
    mon.update(bus, fakeMillis, 2, 26, true);
    assert(mon.side(1).bad == 1 && bus.sent.size() == 3 && (bus.sent[2].id & 0xFF) == 2);
    assert(mon.onReply(reply(2, valuesReply(0, 1.5f)), fakeMillis + 3));

    // A queued probe GET goes first, and its reply is left for main.cpp.
    mon.queueProbe(26, vescprobe::COMM_GET_MCCONF);
    fakeMillis += 5;
    mon.update(bus, fakeMillis, 2, 26, true);
    assert(bus.sent.size() == 4 && (bus.sent[3].id & 0xFF) == 26 && bus.sent[3].buf[2] == vescprobe::COMM_GET_MCCONF);
    assert(mon.busy(fakeMillis));
    assert(!mon.onReply(reply(26, std::vector<uint8_t>(REAL_MCCONF_26, REAL_MCCONF_26 + 484)), fakeMillis + 20));
    assert(!mon.busy(fakeMillis + 20));
    // A CRC failure is not consumed either (main.cpp logs it as before).
    assert(!mon.onReply(reply(26, valuesReply(0, 0), false), fakeMillis + 21));

    // While the settings applier runs (allowed = false) nothing is sent.
    const size_t sent = bus.sent.size();
    for (int i = 0; i < 50; ++i)
      mon.update(bus, fakeMillis += 20, 2, 26, false);
    mon.queueProbe(2, vescprobe::COMM_FW_VERSION);
    mon.update(bus, fakeMillis += 20, 2, 26, false);
    assert(bus.sent.size() == sent);
    mon.update(bus, fakeMillis += 20, 2, 26, true);
    assert(bus.sent.size() == sent + 1 && bus.sent.back().buf[2] == vescprobe::COMM_FW_VERSION);
  }

  // ---- Faults: logged at once, once per change, never dropped for lack of room.
  {
    vescmon::Monitor mon;
    VescBus bus;
    mon.update(bus, 0, 2, 26, true);
    Serial1.output.clear();
    assert(mon.onReply(reply(2, valuesReply(4, -48.7f)), 2000));
    mon.flushFaults();
    const std::string fault = lastLine(Serial1.output, "DBG VFAULT");
    assert(fault.find("DBG VFAULT id=2 code=4 name=ABS_OVER_CURRENT ms=2000 imot=-48.70") == 0);
    assert(fault.find(" n=1 side=L") != std::string::npos);
    // Still faulted on the next poll: no new line.
    assert(mon.onReply(reply(2, valuesReply(4, -10.0f)), 2200));
    mon.flushFaults();
    assert(count(Serial1.output, "DBG VFAULT") == 1);
    // Cleared: one NONE line. The other VESC is tracked separately.
    assert(mon.onReply(reply(26, valuesReply(0, 0)), 2300));
    assert(mon.onReply(reply(2, valuesReply(0, 0)), 2600));
    mon.flushFaults();
    assert(count(Serial1.output, "DBG VFAULT") == 2);
    assert(lastLine(Serial1.output, "DBG VFAULT").find("id=2 code=0 name=NONE") != std::string::npos);
    assert(mon.side(0).faults == 1 && mon.side(0).lastFault == 4 && mon.side(1).faults == 0);

    // No Bluetooth room: the line waits instead of being dropped.
    Serial1.capacity = 20;
    assert(mon.onReply(reply(26, valuesReply(5, 3.0f)), 3000));
    mon.flushFaults();
    assert(count(Serial1.output, "DBG VFAULT") == 2);
    Serial1.capacity = 4096;
    mon.flushFaults();
    assert(count(Serial1.output, "DBG VFAULT") == 3);
    assert(lastLine(Serial1.output, "DBG VFAULT").find("id=26 code=5 name=OVER_TEMP_FET") != std::string::npos);
  }

  // ---- Line formats: DBG VVAL and DBG VESC, worst-case lengths fit logf's buffer.
  {
    vescmon::Monitor mon;
    VescBus bus;
    Serial1.output.clear();
    mon.update(bus, 100, 2, 26, true); // first poll: the left VESC
    mon.logValues(1, 10);              // nothing received yet
    assert(lastLine(Serial1.output, "DBG VVAL") == "DBG VVAL id=26 age=-1 n=0/0/0 side=R");
    assert(mon.onReply(reply(2, valuesReply(0, -41.23f)), 100));
    mon.logValues(0, 180);
    const std::string vval = lastLine(Serial1.output, "DBG VVAL id=2 ");
    assert(vval.find("DBG VVAL id=2 age=80 fault=0 fetC=25.4 motC=-270.0 imot=-41.23 iin=-3.21 iD=0.12 iQ=-41.23 "
                     "duty=-0.456 erpm=-8123 vin=52.3 ah=0.1234/0.0567 wh=6.432/2.500 tach=-123456/654321 ") == 0);
    assert(vval.find(" mos=30.1/30.2/30.3 vD=1.23 vQ=-20.50 st=0 faults=0 last=0 n=1/1/0") != std::string::npos);

    // Extreme values must still fit (logf drops a line that doesn't).
    std::vector<uint8_t> worst(VESC_VALUES_FULL_LEN, 0x80); // most negative everywhere
    worst[0] = VESC_COMM_GET_VALUES;
    assert(mon.onReply(reply(2, worst), 200));
    const size_t before = Serial1.output.size();
    mon.logValues(0, 4000000000u);
    assert(Serial1.output.size() > before);
    const std::string longest = lastLine(Serial1.output, "DBG VVAL id=2 ");
    assert(longest.size() < 400);
    printf("DBG VVAL typical %zu bytes, worst %zu bytes\n", vval.size() + 1, longest.size() + 1);

    VescStatus s{};
    const uint8_t s1[8] = {0xFF, 0xFF, 0xE0, 0xC0, 0xFF, 0x1A, 0x00, 0x7B}; // -8000 erpm, -23.0 A, 0.123
    vescDecodeStatus(VESC_STATUS, s1, 8, s, 1000);
    fakeMillis = 1020;
    vescmon::logStatus(s, 2, 'L', 1020);
    const std::string vesc = lastLine(Serial1.output, "DBG VESC ");
    assert(vesc.find("DBG VESC id=2 age=20 frames=1 erpm=-8000 current=-23.0 duty=0.123 ") == 0);
    assert(vesc.find(" ipk=-23.0 ") != std::string::npos && vesc.find(" ages=20/-1/-1/-1/-1 side=L") != std::string::npos);
    VescStatus never{};
    vescmon::logStatus(never, 26, 'R', 1020);
    assert(lastLine(Serial1.output, "DBG VESC id=26").find("age=-1 frames=0") != std::string::npos);
    printf("DBG VESC typical %zu bytes\n", vesc.size() + 1);
  }

  // ---- Link accounting: a line that doesn't fit is dropped whole and counted.
  {
    const auto dropped = telemetry::Telemetry::link().droppedLines;
    Serial1.capacity = 10;
    telemetry::Telemetry::logf("DBG SYS this line does not fit\n");
    assert(telemetry::Telemetry::link().droppedLines == dropped + 1);
    assert(telemetry::Telemetry::takeMinFree() == 10);
    Serial1.capacity = 4096;
  }

  // ---- vescSend counts commands the CAN driver refused.
  {
    VescBus bus;
    bus.full = true;
    const uint32_t before = vescTxFailures();
    vescSetCurrent(bus, 2, 0.0f);
    assert(vescTxFailures() == before + 1);
  }

  printf("vesc monitor: status frames, real config reassembly, poll schedule, faults, line formats\n");
  return 0;
}
