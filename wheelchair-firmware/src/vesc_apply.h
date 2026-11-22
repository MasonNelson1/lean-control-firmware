#pragma once
#include "vesc_probe.h"
#include "vesc_apply_data.h"
#include "telemetry/Telemetry.h"

// ============================================================
// One-shot VESC settings write over CAN (approved by Charles 2026-11-22).
// For each VESC and each config (motor, app):
//   1. GET the config.
//   2. If it already equals AFTER: nothing to do. If it equals BEFORE byte for
//      byte: SET AFTER. Anything else: abort that job and report (the VESC holds
//      settings we haven't reviewed).
//   3. GET again and check it now equals AFTER.
// Runs only in DISARM, only once per boot, only with VESC_APPLY_ENABLED.
// Every step is reported as a DBG VAPPLY line.
// ============================================================

namespace vescapply
{
constexpr bool VESC_APPLY_ENABLED = true;
constexpr uint8_t COMM_SET_MCCONF = 13;
constexpr uint8_t COMM_SET_APPCONF = 16;
constexpr uint32_t REPLY_TIMEOUT_MS = 3000; // flash write on SET can take a moment
constexpr uint8_t MAX_TRIES = 3;

struct Job
{
  const char *name;
  bool left;
  bool motor; // mcconf (true) or appconf
  const uint8_t *before;
  const uint8_t *after;
  uint16_t len; // config bytes, without the command byte
};

enum class Step : uint8_t
{
  Idle,
  Get,
  WaitGet,
  Send,
  WaitSet,
  Verify,
  WaitVerify,
  Next,
  Done
};

// Sends one long buffer the way VESC's comm_can_send_buffer does: FILL_RX_BUFFER
// frames (7 bytes, offsets < 256), FILL_RX_BUFFER_LONG frames (6 bytes), then
// PROCESS_RX_BUFFER with length and CRC. Non-blocking; call pump() each loop.
struct LongSender
{
  uint8_t data[1024];
  uint16_t len = 0, pos = 0;
  uint8_t target = 0;
  bool fillsDone = false, processSent = true;
  uint32_t fillsDoneMs = 0;

  void start(uint8_t vescId, const uint8_t *payload, uint16_t n)
  {
    memcpy(data, payload, n);
    len = n;
    pos = 0;
    target = vescId;
    fillsDone = false;
    processSent = false;
  }
  bool busy() const { return !processSent; }

  void pump(VescBus &bus)
  {
    if (processSent)
      return;
    for (int k = 0; k < 8 && !fillsDone; ++k)
    {
      CAN_message_t m;
      m.flags.extended = 1;
      if (pos < 256 && pos <= 252)
      {
        const uint16_t n = min<uint16_t>(7, len - pos);
        m.id = target | ((uint32_t)vescprobe::CAN_FILL_RX_BUFFER << 8);
        m.buf[0] = (uint8_t)pos;
        memcpy(&m.buf[1], data + pos, n);
        m.len = n + 1;
        if (!bus.write(m))
          return; // TX full; try again next loop
        pos += n;
      }
      else
      {
        const uint16_t n = min<uint16_t>(6, len - pos);
        m.id = target | ((uint32_t)vescprobe::CAN_FILL_RX_BUFFER_LONG << 8);
        m.buf[0] = pos >> 8;
        m.buf[1] = pos & 0xFF;
        memcpy(&m.buf[2], data + pos, n);
        m.len = n + 2;
        if (!bus.write(m))
          return;
        pos += n;
      }
      if (pos >= len)
      {
        fillsDone = true;
        fillsDoneMs = millis();
      }
    }
    // Give queued fill frames time to leave before the PROCESS frame.
    if (fillsDone && millis() - fillsDoneMs >= 30)
    {
      const uint16_t crc = vescprobe::crc16(data, len);
      CAN_message_t m;
      m.flags.extended = 1;
      m.id = target | ((uint32_t)vescprobe::CAN_PROCESS_RX_BUFFER << 8);
      m.buf[0] = vescprobe::PROBE_CAN_ID;
      m.buf[1] = 0; // process and reply
      m.buf[2] = len >> 8;
      m.buf[3] = len & 0xFF;
      m.buf[4] = crc >> 8;
      m.buf[5] = crc & 0xFF;
      m.len = 6;
      if (bus.write(m))
        processSent = true;
    }
  }
};

class Applier
{
public:

  bool active() const { return step_ != Step::Idle && step_ != Step::Done; }
  bool finished() const { return step_ == Step::Done; }

  // Start once both VESCs are reporting and the chair is disarmed.
  void maybeStart(bool bothFresh, bool disarmed, uint8_t leftId, uint8_t rightId)
  {
    leftId_ = leftId;
    rightId_ = rightId;
    if (!VESC_APPLY_ENABLED || step_ != Step::Idle || !bothFresh || !disarmed)
      return;
    if (firstOkMs_ == 0)
      firstOkMs_ = millis();
    if (millis() - firstOkMs_ < 3000) // let both VESCs finish booting
      return;
    job_ = 0;
    step_ = Step::Get;
    telemetry::Telemetry::logf("DBG VAPPLY start jobs=%u\n", (unsigned)JOBS);
  }

  // Abort if the chair leaves DISARM mid-run (it can't while the seat is empty, but be sure).
  void update(VescBus &bus, bool disarmed)
  {
    if (!active())
      return;
    if (!disarmed)
    {
      report("abort", "chair left DISARM");
      step_ = Step::Done;
      return;
    }
    sender_.pump(bus);
    const Job &j = jobs()[job_];
    const uint8_t id = j.left ? leftId_ : rightId_;
    switch (step_)
    {
    case Step::Get:
    case Step::Verify:
      vescprobe::ask(bus, id, j.motor ? vescprobe::COMM_GET_MCCONF : vescprobe::COMM_GET_APPCONF);
      waitSince_ = millis();
      step_ = step_ == Step::Get ? Step::WaitGet : Step::WaitVerify;
      break;
    case Step::Send:
    {
      static uint8_t payload[1024];
      payload[0] = j.motor ? COMM_SET_MCCONF : COMM_SET_APPCONF;
      memcpy(payload + 1, j.after, j.len);
      sender_.start(id, payload, j.len + 1);
      waitSince_ = millis();
      step_ = Step::WaitSet;
      break;
    }
    case Step::WaitGet:
    case Step::WaitSet:
    case Step::WaitVerify:
      if (millis() - waitSince_ > REPLY_TIMEOUT_MS)
      {
        if (++tries_ >= MAX_TRIES)
        {
          report("fail", "no reply");
          step_ = Step::Next;
        }
        else
        {
          report("retry", "timeout");
          step_ = step_ == Step::WaitGet ? Step::Get : step_ == Step::WaitSet ? Step::Send : Step::Verify;
        }
      }
      break;
    case Step::Next:
      tries_ = 0;
      if (++job_ >= JOBS)
      {
        step_ = Step::Done;
        telemetry::Telemetry::logf("DBG VAPPLY done\n");
      }
      else
        step_ = Step::Get;
      break;
    default:
      break;
    }
  }

  // Feed every complete CAN reply here while active().
  void onReply(const vescprobe::Reply &r)
  {
    if (!active() || !r.crcOk || r.len == 0)
      return;
    const Job &j = jobs()[job_];
    const uint8_t id = j.left ? leftId_ : rightId_;
    if (r.fromId != id)
      return;
    const uint8_t getCmd = j.motor ? vescprobe::COMM_GET_MCCONF : vescprobe::COMM_GET_APPCONF;
    const uint8_t setCmd = j.motor ? COMM_SET_MCCONF : COMM_SET_APPCONF;
    if (step_ == Step::WaitGet && r.data[0] == getCmd)
    {
      if (r.len != j.len + 1)
      {
        report("abort", "unexpected length");
        step_ = Step::Next;
      }
      else if (memcmp(r.data + 1, j.after, j.len) == 0)
      {
        report("ok", "already applied");
        step_ = Step::Next;
      }
      else if (memcmp(r.data + 1, j.before, j.len) == 0)
      {
        report("write", "config matches the reviewed copy; writing");
        tries_ = 0;
        step_ = Step::Send;
      }
      else
      {
        uint16_t i = 0;
        while (i < j.len && r.data[1 + i] == j.before[i])
          ++i;
        telemetry::Telemetry::logf("DBG VAPPLY job=%s result=abort why=config differs from the reviewed copy at byte %u\n", j.name, i);
        step_ = Step::Next;
      }
    }
    else if (step_ == Step::WaitSet && r.data[0] == setCmd)
    {
      report("written", "VESC acknowledged");
      tries_ = 0;
      step_ = Step::Verify;
    }
    else if (step_ == Step::WaitVerify && r.data[0] == getCmd)
    {
      const bool ok = r.len == j.len + 1 && memcmp(r.data + 1, j.after, j.len) == 0;
      report(ok ? "verified" : "VERIFY-FAILED", ok ? "read back equals the approved settings" : "read back differs");
      step_ = Step::Next;
    }
  }

private:
  static constexpr uint8_t JOBS = 4;
  static const Job *jobs()
  {
    static const Job j[JOBS] = {
        {"left-motor", true, true, VESC_MC_BEFORE_L, VESC_MC_AFTER_L, sizeof(VESC_MC_BEFORE_L)},
        {"left-app", true, false, VESC_APP_BEFORE_L, VESC_APP_AFTER_L, sizeof(VESC_APP_BEFORE_L)},
        {"right-motor", false, true, VESC_MC_BEFORE_R, VESC_MC_AFTER_R, sizeof(VESC_MC_BEFORE_R)},
        {"right-app", false, false, VESC_APP_BEFORE_R, VESC_APP_AFTER_R, sizeof(VESC_APP_BEFORE_R)},
    };
    return j;
  }
  void report(const char *result, const char *why)
  {
    telemetry::Telemetry::logf("DBG VAPPLY job=%s result=%s why=%s\n", jobs()[job_].name, result, why);
  }

  uint8_t leftId_ = 0, rightId_ = 0;
  Step step_ = Step::Idle;
  uint8_t job_ = 0, tries_ = 0;
  uint32_t waitSince_ = 0, firstOkMs_ = 0;
  LongSender sender_;
};
} // namespace vescapply
