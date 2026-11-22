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
  const uint8_t *after;   // app config: the full approved bytes
  uint16_t len;           // config bytes, without the command byte
  const VescPatch *patch; // motor config: approved fields patched onto the live config
  uint8_t npatch;
};

// Bytes the VESC re-measures at every boot (motor config only); ignored when comparing.
inline bool isVolatile(bool motor, uint16_t i)
{
  if (!motor)
    return false;
  for (const auto &v : VESC_MC_VOLATILE)
    if (i >= v[0] && i < v[0] + v[1])
      return true;
  return false;
}
// First byte where a and b differ (ignoring boot-measured bytes), or len if equal.
inline uint16_t firstDiff(bool motor, const uint8_t *a, const uint8_t *b, uint16_t len)
{
  for (uint16_t i = 0; i < len; ++i)
    if (a[i] != b[i] && !isVolatile(motor, i))
      return i;
  return len;
}

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
  uint16_t framesSent = 0, writeFails = 0;
  uint32_t lastFrameUs = 0;

  void start(uint8_t vescId, const uint8_t *payload, uint16_t n)
  {
    memcpy(data, payload, n);
    len = n;
    pos = 0;
    target = vescId;
    fillsDone = false;
    processSent = false;
    framesSent = 0;
    writeFails = 0;
  }
  bool busy() const { return !processSent; }

  void pump(VescBus &bus)
  {
    if (processSent)
      return;
    // One frame at a time, 2 ms apart: VESC 6.06 drops the whole buffer if a fill
    // frame arrives out of order (rx_buffer_offset must match), and several frames
    // queued in different FlexCAN mailboxes can leave in mailbox order, not queue order.
    if (!fillsDone && micros() - lastFrameUs < 2000)
      return;
    for (int k = 0; k < 1 && !fillsDone; ++k)
    {
      lastFrameUs = micros();
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
        {
          ++writeFails;
          return; // TX full; try again next loop
        }
        ++framesSent;
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
        {
          ++writeFails;
          return;
        }
        ++framesSent;
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
    if (millis() - firstOkMs_ < 30000) // let both VESCs boot and the Bluetooth tap reconnect
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
      memcpy(payload + 1, target_, j.len);
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
      const uint8_t *live = r.data + 1;
      if (r.len != j.len + 1)
      {
        report("abort", "unexpected length");
        step_ = Step::Next;
        return;
      }
      // What we would write: the approved bytes (app) or the live config with the approved fields patched in (motor).
      if (j.patch)
      {
        memcpy(target_, live, j.len);
        for (uint8_t k = 0; k < j.npatch; ++k)
          memcpy(target_ + j.patch[k].off, j.patch[k].bytes, j.patch[k].len);
      }
      else
        memcpy(target_, j.after, j.len);
      if (firstDiff(j.motor, live, target_, j.len) == j.len)
      {
        report("ok", "already applied");
        step_ = Step::Next;
        return;
      }
      const uint16_t d = firstDiff(j.motor, live, j.before, j.len);
      if (d == j.len)
      {
        report("write", "config matches the reviewed copy; writing");
        tries_ = 0;
        step_ = Step::Send;
      }
      else
      {
        telemetry::Telemetry::logf("DBG VAPPLY job=%s result=abort why=config differs from the reviewed copy at byte %u\n", j.name, d);
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
      const bool ok = r.len == j.len + 1 && firstDiff(j.motor, r.data + 1, target_, j.len) == j.len;
      report(ok ? "verified" : "VERIFY-FAILED", ok ? "read back equals the approved settings" : "read back differs");
      step_ = Step::Next;
    }
  }

private:
  static constexpr uint8_t JOBS = 4;
  static const Job *jobs()
  {
    static const Job j[JOBS] = {
        {"left-motor", true, true, VESC_MC_BEFORE_L, nullptr, sizeof(VESC_MC_BEFORE_L), VESC_MC_PATCH_L, sizeof(VESC_MC_PATCH_L) / sizeof(VescPatch)},
        {"left-app", true, false, VESC_APP_BEFORE_L, VESC_APP_AFTER_L, sizeof(VESC_APP_BEFORE_L), nullptr, 0},
        {"right-motor", false, true, VESC_MC_BEFORE_R, nullptr, sizeof(VESC_MC_BEFORE_R), VESC_MC_PATCH_R, sizeof(VESC_MC_PATCH_R) / sizeof(VescPatch)},
        {"right-app", false, false, VESC_APP_BEFORE_R, VESC_APP_AFTER_R, sizeof(VESC_APP_BEFORE_R), nullptr, 0},
    };
    return j;
  }
  static const char *stepName(Step s)
  {
    switch (s)
    {
    case Step::Get: return "get";
    case Step::WaitGet: return "wait-get";
    case Step::Send: return "send";
    case Step::WaitSet: return "wait-set";
    case Step::Verify: return "verify";
    case Step::WaitVerify: return "wait-verify";
    default: return "-";
    }
  }
  void report(const char *result, const char *why)
  {
    char line[96];
    snprintf(line, sizeof(line), "job=%s step=%s result=%s why=%s", jobs()[job_].name, stepName(step_), result, why);
    telemetry::Telemetry::logf("DBG VAPPLY %s fills=%u wfail=%u proc=%d\n", line, sender_.framesSent, sender_.writeFails, sender_.processSent);
    if (nlog_ < 12)
      snprintf(log_[nlog_++], sizeof(log_[0]), "%s", line);
  }

public:
  // After the run, repeat every result every 10 s so a reconnecting tap still sees them.
  void repeatSummary()
  {
    static uint32_t last = 0;
    if (step_ != Step::Done || millis() - last < 10000)
      return;
    last = millis();
    for (uint8_t i = 0; i < nlog_; ++i)
      telemetry::Telemetry::logf("DBG VAPPLY summary %u/%u %s\n", i + 1, nlog_, log_[i]);
  }

private:
  char log_[12][96] = {};
  uint8_t nlog_ = 0;

  uint8_t leftId_ = 0, rightId_ = 0;
  Step step_ = Step::Idle;
  uint8_t job_ = 0, tries_ = 0;
  uint32_t waitSince_ = 0, firstOkMs_ = 0;
  LongSender sender_;
  uint8_t target_[1024] = {};
};
} // namespace vescapply
