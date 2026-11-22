#pragma once
#include "vesc_probe.h"
#include "telemetry/Telemetry.h"
#include <stdio.h>
#include <string.h>

// ============================================================
// Read-only VESC health monitor (telemetry only; never commands a motor).
//
// Polls COMM_GET_VALUES over CAN (short-buffer GET, reply to PROBE_CAN_ID),
// alternating left and right, and reports:
//   DBG VVAL   once a second per VESC: the full values, including the fault code
//   DBG VFAULT at once, whenever a VESC's fault code changes (to a fault or back to NONE)
//   DBG VESC   once a second per VESC: the CAN status frames 1-5 (vesc_decode.h)
//
// It also owns the one-at-a-time rule for every GET over CAN. A long reply
// arrives as FILL_RX_BUFFER frames that carry no sender id, so two VESCs
// answering at once would mix their bytes (the CRC then rejects both). The
// probe in main.cpp queues its GET here instead of sending it directly, and
// nothing is asked while vescApplier (vesc_apply.h) is active.
// ============================================================

namespace vescmon
{
// A VESC holds a fault code for m_fault_stop_time_ms (500 ms on both of ours)
// and then clears it. Ten polls a second, alternating, asks each VESC every
// 200 ms, so a fault is seen at least twice even if one poll is skipped for a
// probe or a lost reply. 5 Hz (400 ms per VESC) could miss it after one skip.
constexpr uint32_t POLL_PERIOD_MS = 100;
// Wait this long for a reply before asking again (a 484-byte config reply
// takes ≈ 20 ms on the bus; a values reply ≈ 3 ms).
constexpr uint32_t REPLY_WAIT_MS = 150;

struct Side
{
  VescValues values{};
  bool have = false;      // values holds a decoded reply
  uint32_t lastMs = 0;    // millis() of that reply
  uint32_t asked = 0;     // GET_VALUES sent
  uint32_t replies = 0;   // GET_VALUES replies decoded
  uint32_t bad = 0;       // replies lost: CRC failure, short, or no answer within REPLY_WAIT_MS
  uint32_t faults = 0;    // fault episodes (code changed to non-zero)
  uint8_t lastFault = 0;  // most recent non-zero fault code (0 = none seen since boot)
  uint32_t lastFaultMs = 0;
  uint32_t latencyMaxMs = 0; // slowest reply since the last DBG VVAL
};

class Monitor
{
public:
  // A probe GET (firmware version, motor or app config) for when the bus is free.
  void queueProbe(uint8_t vescId, uint8_t comm)
  {
    probeId_ = vescId;
    probeComm_ = comm;
    probePending_ = true;
  }

  // True while a reply we asked for may still be arriving.
  bool busy(uint32_t now) const { return outstanding_ && now - askedMs_ < REPLY_WAIT_MS; }

  // Call every loop. allowed = false (applier active) sends nothing.
  void update(VescBus &bus, uint32_t now, uint8_t leftId, uint8_t rightId, bool allowed)
  {
    ids_[0] = leftId;
    ids_[1] = rightId;
    if (outstanding_ && !busy(now))
    {
      outstanding_ = false; // timed out
      if (outstandingSide_ >= 0)
        ++sides_[outstandingSide_].bad;
    }
    if (!allowed || outstanding_)
      return;
    if (probePending_)
    {
      probePending_ = false;
      send(bus, probeId_, probeComm_, now, -1);
      return;
    }
    if (now - lastPollMs_ < POLL_PERIOD_MS)
      return;
    lastPollMs_ = now;
    next_ ^= 1;
    ++sides_[next_].asked;
    send(bus, ids_[next_], VESC_COMM_GET_VALUES, now, next_);
  }

  // Every complete reply addressed to us (main.cpp's CAN loop). Returns true
  // when it was a values reply, which this consumes; anything else is the
  // probe's and main.cpp logs it as before.
  bool onReply(const vescprobe::Reply &r, uint32_t now)
  {
    // Any finished reply means nobody is mid-answer: the bus is free again.
    const bool waited = outstanding_;
    outstanding_ = false;
    const int side = r.fromId == ids_[0] ? 0 : r.fromId == ids_[1] ? 1 : -1;
    if (!r.crcOk || r.len == 0)
    {
      // Can't tell whose it was; charge it to the values poll we were waiting on.
      if (waited && outstandingSide_ >= 0)
        ++sides_[outstandingSide_].bad;
      return false;
    }
    if (r.data[0] != VESC_COMM_GET_VALUES)
      return false;
    if (side < 0)
      return true;
    Side &s = sides_[side];
    VescValues v;
    if (!vescDecodeValues(r.data, r.len, v))
    {
      ++s.bad;
      return true;
    }
    if (waited && side == outstandingSide_ && now - askedMs_ > s.latencyMaxMs)
      s.latencyMaxMs = now - askedMs_;
    const uint8_t previous = s.have ? s.values.fault : 0;
    s.values = v;
    s.have = true;
    s.lastMs = now;
    ++s.replies;
    if (v.fault != previous)
    {
      if (v.fault)
      {
        ++s.faults;
        s.lastFault = v.fault;
        s.lastFaultMs = now;
      }
      queueFault(side, now);
    }
    return true;
  }

  // Writes queued DBG VFAULT lines as soon as Bluetooth has room for them.
  // Call every loop; a fault line is never dropped for lack of space.
  void flushFaults()
  {
    while (faultCount_ > 0)
    {
      const char *line = faultLines_[faultHead_];
      if (!telemetry::Telemetry::hasRoom(strlen(line)))
        return;
      telemetry::Telemetry::logf("%s", line);
      faultHead_ = (faultHead_ + 1) % MaxFaultLines;
      --faultCount_;
    }
  }

  const Side &side(int i) const { return sides_[i]; }

  // DBG VVAL: the latest values reply (side = L or R). Averages (imot, iin, iD, iQ, vD, vQ) are
  // means since the previous poll of that VESC. n = asked/replies/lost.
  void logValues(int i, uint32_t now)
  {
    Side &s = sides_[i];
    const VescValues &v = s.values;
    if (!s.have)
    {
      telemetry::Telemetry::logf("DBG VVAL id=%u age=-1 n=%lu/%lu/%lu side=%c\n", ids_[i], (unsigned long)s.asked,
                                 (unsigned long)s.replies, (unsigned long)s.bad, i == 0 ? 'L' : 'R');
      return;
    }
    telemetry::Telemetry::logf(
        "DBG VVAL id=%u age=%lu fault=%u fetC=%.1f motC=%.1f imot=%.2f iin=%.2f iD=%.2f iQ=%.2f duty=%.3f "
        "erpm=%.0f vin=%.1f ah=%.4f/%.4f wh=%.3f/%.3f tach=%ld/%ld pid=%.2f mos=%.1f/%.1f/%.1f vD=%.2f vQ=%.2f "
        "st=%u faults=%lu last=%u n=%lu/%lu/%lu lat=%lu side=%c\n",
        ids_[i], (unsigned long)(now - s.lastMs), v.fault, v.tempFet, v.tempMotor, v.currentMotor, v.currentIn,
        v.currentD, v.currentQ, v.duty, v.erpm, v.inputVoltage, v.ampHours, v.ampHoursCharged, v.wattHours,
        v.wattHoursCharged, (long)v.tachometer, (long)v.tachometerAbs, v.pidPos, v.tempMos[0], v.tempMos[1],
        v.tempMos[2], v.voltageD, v.voltageQ, v.status, (unsigned long)s.faults, s.lastFault,
        (unsigned long)s.asked, (unsigned long)s.replies, (unsigned long)s.bad, (unsigned long)s.latencyMaxMs,
        i == 0 ? 'L' : 'R');
    s.latencyMaxMs = 0;
  }

private:
  static constexpr uint8_t MaxFaultLines = 4;

  void send(VescBus &bus, uint8_t vescId, uint8_t comm, uint32_t now, int side)
  {
    vescprobe::ask(bus, vescId, comm);
    outstanding_ = true;
    outstandingSide_ = side;
    askedMs_ = now;
  }

  void queueFault(int i, uint32_t now)
  {
    const VescValues &v = sides_[i].values;
    if (faultCount_ == MaxFaultLines) // keep the newest
    {
      faultHead_ = (faultHead_ + 1) % MaxFaultLines;
      --faultCount_;
    }
    char *line = faultLines_[(faultHead_ + faultCount_) % MaxFaultLines];
    snprintf(line, sizeof(faultLines_[0]),
             "DBG VFAULT id=%u code=%u name=%s ms=%lu imot=%.2f iin=%.2f erpm=%.0f duty=%.3f vin=%.1f fetC=%.1f n=%lu side=%c\n",
             ids_[i], v.fault, vescFaultName(v.fault), (unsigned long)now, v.currentMotor, v.currentIn, v.erpm,
             v.duty, v.inputVoltage, v.tempFet, (unsigned long)sides_[i].faults, i == 0 ? 'L' : 'R');
    ++faultCount_;
  }

  Side sides_[2];
  uint8_t ids_[2] = {0, 0};
  int next_ = 1; // the first poll goes to the left VESC
  uint32_t lastPollMs_ = 0;
  bool outstanding_ = false;
  int outstandingSide_ = -1; // -1 = a probe GET
  uint32_t askedMs_ = 0;
  bool probePending_ = false;
  uint8_t probeId_ = 0, probeComm_ = 0;
  char faultLines_[MaxFaultLines][160];
  uint8_t faultHead_ = 0, faultCount_ = 0;
};

// DBG VESC: one VESC's CAN status frames. age = ms since any status frame
// (-1 = never); ages = per frame type STATUS_1..5 (-1 = never received, so a
// type the VESC doesn't send shows -1). ipk/iinpk = largest motor/input current
// since the previous line. side = L or R (which wheel). Clears those peaks.
inline void logStatus(VescStatus &v, uint8_t id, char side, uint32_t now)
{
  long ages[5];
  for (int i = 0; i < 5; ++i)
    ages[i] = v.typeFrames[i] ? (long)(now - v.typeMs[i]) : -1L;
  telemetry::Telemetry::logf(
      "DBG VESC id=%u age=%ld frames=%lu erpm=%ld current=%.1f duty=%.3f fetC=%.1f vin=%.1f motC=%.1f iin=%.1f "
      "ipk=%.1f iinpk=%.1f ah=%.4f/%.4f wh=%.3f/%.3f tach=%ld pid=%.1f ages=%ld/%ld/%ld/%ld/%ld side=%c\n",
      id, v.frames ? (long)(now - v.lastMs) : -1L, (unsigned long)v.frames, (long)v.erpm, v.current, v.duty,
      v.mosfetTempC, v.inputVoltage, v.motorTempC, v.inputCurrent, v.currentPeak, v.inputCurrentPeak, v.ampHours,
      v.ampHoursCharged, v.wattHours, v.wattHoursCharged, (long)v.tachometer, v.pidPos, ages[0], ages[1], ages[2],
      ages[3], ages[4], side);
  vescClearPeaks(v);
}
} // namespace vescmon
