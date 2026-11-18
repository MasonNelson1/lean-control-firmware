#pragma once

#include <stdint.h>

#include "drive_constants.h"

// Speed and acceleration selector knobs (C&K A10403RNCQ: 4-position SP4T,
// non-shorting / break-before-make). Each knob's common goes to one analog pin
// with a 4.7 kOhm pull-up to 3.3 V; each position grounds it through its own
// resistor (1k, 2.2k, 4.7k, 10k). Nominal 12-bit readings: 718, 1306, 2048,
// 2786. Open (unplugged cable, or between detents) reads ~4095 and is invalid.
//
// Pure logic, no Arduino calls, so tests/native/drive_knobs_test.cpp can run it.
namespace knobs
{
    constexpr int Positions = 4;
    constexpr int Invalid = -1;

    // Band edges are midpoints between neighbouring nominal readings. Below
    // MinValidRaw means the common is shorted to GND; above the last band edge
    // means open. Retune here if bench readings drift.
    constexpr uint16_t MinValidRaw = 300;
    constexpr uint16_t BandMax[Positions] = {1011, 1676, 2416, 3440};

    constexpr uint8_t StableReads = 3;    // matching reads needed to accept a position
    constexpr uint32_t FaultHoldMs = 200; // invalid longer than this -> fault (slowest)
    constexpr uint32_t SampleMs = 20;     // sampling interval used by main.cpp

    // Position 1 (index 0) is the gentlest. Fractions of the design limits, so
    // every selection stays at or below MAX_SPEED / MAX_ACCEL.
    constexpr float SpeedFraction[Positions] = {0.25f, 0.50f, 0.75f, 1.00f};
    constexpr float AccelFraction[Positions] = {0.25f, 0.50f, 0.75f, 1.00f};

    inline float speedLimitMS(int position) { return SpeedFraction[position] * common::MAX_SPEED; }
    inline float accelLimitMS2(int position) { return AccelFraction[position] * common::MAX_ACCEL; }

    // Raw 12-bit reading -> position index 0..3, or Invalid.
    inline int classify(uint16_t raw)
    {
        if (raw < MinValidRaw)
            return Invalid;
        for (int i = 0; i < Positions; ++i)
            if (raw <= BandMax[i])
                return i;
        return Invalid;
    }

    /**
     * Debounced reader for one knob. Call update() every SampleMs.
     * - A new position is accepted after StableReads identical valid reads.
     * - Invalid reads shorter than FaultHoldMs keep the last good position
     *   (rides through the open gap between non-shorting detents).
     * - Invalid for FaultHoldMs or longer: faulted, position falls back to 0
     *   (slowest) until StableReads matching valid reads arrive again.
     * Starts at position 0, not faulted.
     */
    class RotarySwitch
    {
    public:
        // Accept a valid power-on reading immediately, without debouncing.
        void seed(uint16_t raw, uint32_t nowMs)
        {
            raw_ = raw;
            const int p = classify(raw);
            if (p == Invalid)
            {
                invalid_ = true;
                invalidSinceMs_ = nowMs;
                return;
            }
            position_ = p;
            candidate_ = p;
            count_ = StableReads;
        }

        // Returns true when position() or faulted() changed.
        bool update(uint16_t raw, uint32_t nowMs)
        {
            raw_ = raw;
            const int previousPosition = position_;
            const bool previousFault = faulted_;
            const int p = classify(raw);

            if (p == Invalid)
            {
                candidate_ = Invalid;
                count_ = 0;
                if (!invalid_)
                {
                    invalid_ = true;
                    invalidSinceMs_ = nowMs;
                }
                if (nowMs - invalidSinceMs_ >= FaultHoldMs)
                {
                    faulted_ = true;
                    position_ = 0;
                }
            }
            else
            {
                invalid_ = false;
                if (p == candidate_)
                {
                    if (count_ < StableReads)
                        ++count_;
                }
                else
                {
                    candidate_ = p;
                    count_ = 1;
                }
                if (count_ >= StableReads)
                {
                    position_ = candidate_;
                    faulted_ = false;
                }
            }
            return position_ != previousPosition || faulted_ != previousFault;
        }

        int position() const { return position_; } // always 0..Positions-1
        bool faulted() const { return faulted_; }
        uint16_t lastRaw() const { return raw_; }

    private:
        int position_ = 0;
        int candidate_ = Invalid;
        uint8_t count_ = 0;
        bool invalid_ = false;
        bool faulted_ = false;
        uint32_t invalidSinceMs_ = 0;
        uint16_t raw_ = 0;
    };

    // Raise immediately to target; lower no faster than ratePerS. Used so a
    // speed-knob reduction while moving never steps the speed cap down.
    inline float slewDown(float current, float target, float ratePerS, float dt)
    {
        if (target >= current)
            return target;
        const float lowered = current - ratePerS * dt;
        return lowered > target ? lowered : target;
    }
} // namespace knobs
