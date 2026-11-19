// Host test (does not command hardware):
// g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/drive_knobs_test.cpp -o /tmp/drive_knobs_test
// /tmp/drive_knobs_test
#include "drive_knobs.h"
#include <cassert>
#include <cmath>
#include <initializer_list>

using namespace knobs;

// Feed one reading per SampleMs, starting at *t. Returns the last update() result.
static bool feed(RotarySwitch &k, uint16_t raw, int count, uint32_t &t) {
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    changed = k.update(raw, t);
    t += SampleMs;
  }
  return changed;
}

int main() {
  // Nominal readings land in their own position.
  assert(classify(718) == 0);
  assert(classify(1306) == 1);
  assert(classify(2048) == 2);
  assert(classify(2786) == 3);
  // Band edges.
  assert(classify(MinValidRaw - 1) == Invalid);
  assert(classify(MinValidRaw) == 0);
  for (int i = 0; i < Positions; ++i) {
    assert(classify(BandMax[i]) == i);
    assert(classify(BandMax[i] + 1) == (i + 1 < Positions ? i + 1 : Invalid));
  }
  // Open and shorted.
  assert(classify(4095) == Invalid);
  assert(classify(0) == Invalid);
  // 5% resistor tolerance on both the pull-up and the position resistor.
  const float nominal[Positions] = {1000.0f, 2200.0f, 4700.0f, 10000.0f};
  for (int i = 0; i < Positions; ++i) {
    for (float pull : {4700.0f * .95f, 4700.0f * 1.05f}) {
      for (float r : {nominal[i] * .95f, nominal[i] * 1.05f}) {
        const uint16_t raw = static_cast<uint16_t>(4095.0f * r / (pull + r));
        assert(classify(raw) == i);
      }
    }
  }

  // Tables are monotonic and capped at the design limits.
  for (int i = 0; i < Positions; ++i) {
    assert(speedLimitMS(i) > 0.0f && speedLimitMS(i) <= common::MAX_SPEED);
    assert(accelLimitMS2(i) > 0.0f && accelLimitMS2(i) <= common::MAX_ACCEL);
    if (i > 0) {
      assert(speedLimitMS(i) > speedLimitMS(i - 1));
      assert(accelLimitMS2(i) > accelLimitMS2(i - 1));
    }
  }
  assert(speedLimitMS(Positions - 1) == common::MAX_SPEED);
  assert(accelLimitMS2(Positions - 1) == common::MAX_ACCEL);

  // Default state is the slowest position, not faulted.
  {
    RotarySwitch k;
    assert(k.position() == 0 && !k.faulted());
  }

  // Seed: a valid power-on reading is accepted immediately.
  {
    RotarySwitch k;
    k.seed(2048, 0);
    assert(k.position() == 2 && !k.faulted());
  }

  // Seed: an invalid power-on reading stays slowest and faults after the hold.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(4095, t);
    assert(k.position() == 0 && !k.faulted());
    t += SampleMs;
    feed(k, 4095, (FaultHoldMs / SampleMs) - 2, t);
    assert(!k.faulted());
    feed(k, 4095, 2, t);
    assert(k.faulted() && k.position() == 0);
  }

  // A new position needs StableReads matching reads.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(718, t);
    for (int i = 1; i < StableReads; ++i) {
      assert(!k.update(2786, t));
      assert(k.position() == 0);
      t += SampleMs;
    }
    assert(k.update(2786, t));
    assert(k.position() == 3);
  }

  // A one-sample glitch to another band is ignored.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(1306, t);
    feed(k, 2786, 1, t);
    feed(k, 1306, 5, t);
    assert(k.position() == 1);
  }

  // Non-shorting detent gap: brief open readings keep the last position,
  // then the new position is accepted.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(1306, t);
    feed(k, 4095, 3, t);  // 60 ms open while turning
    assert(k.position() == 1 && !k.faulted());
    feed(k, 2048, StableReads, t);
    assert(k.position() == 2 && !k.faulted());
  }

  // Sustained open: fault to slowest, report the change once, then recover.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(2786, t);
    t += SampleMs;
    bool reported = false;
    int reports = 0;
    for (uint32_t elapsed = 0; elapsed < FaultHoldMs * 3; elapsed += SampleMs) {
      if (k.update(4095, t)) {
        reported = true;
        ++reports;
      }
      t += SampleMs;
    }
    assert(reported && reports == 1);
    assert(k.faulted() && k.position() == 0);
    // Recovery requires StableReads valid matching reads.
    for (int i = 1; i < StableReads; ++i) {
      k.update(2786, t);
      t += SampleMs;
      assert(k.faulted() && k.position() == 0);
    }
    assert(k.update(2786, t));
    assert(!k.faulted() && k.position() == 3);
  }

  // Shorted to GND faults the same way.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(2048, t);
    t += SampleMs;
    feed(k, 0, FaultHoldMs / SampleMs + 1, t);
    assert(k.faulted() && k.position() == 0);
  }

  // Intermittent opens shorter than the hold never fault.
  {
    RotarySwitch k;
    uint32_t t = 0;
    k.seed(2786, t);
    for (int cycle = 0; cycle < 50; ++cycle) {
      feed(k, 4095, 4, t);
      feed(k, 2786, 1, t);
    }
    assert(!k.faulted() && k.position() == 3);
  }

  // millis() wraparound does not fault early.
  {
    RotarySwitch k;
    uint32_t t = 0xFFFFFFFFu - 50;
    k.seed(2048, t);
    feed(k, 4095, 4, t);  // crosses the wrap
    assert(!k.faulted());
  }

  // slewDown: rises instantly, falls at the given rate, never undershoots.
  assert(slewDown(1.0f, 3.0f, 2.0f, .005f) == 3.0f);
  assert(std::fabs(slewDown(3.0f, 1.0f, 2.0f, .005f) - 2.99f) < 1e-6f);
  assert(slewDown(1.005f, 1.0f, 2.0f, .005f) == 1.0f);
  {
    float limit = common::MAX_SPEED;
    const float target = speedLimitMS(0);
    int steps = 0;
    while (limit > target) {
      const float next = slewDown(limit, target, common::MAX_ACCEL, .005f);
      assert(limit - next <= common::MAX_ACCEL * .005f + 1e-6f);
      assert(next >= target);
      limit = next;
      ++steps;
    }
    assert(steps > 1);
  }
}
