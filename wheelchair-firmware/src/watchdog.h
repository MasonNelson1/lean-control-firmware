#pragma once
#include <Arduino.h>

// Hardware watchdog (i.MX RT1062 WDOG1). If loop() stops running for
// WATCHDOG_TIMEOUT_S the Teensy resets and comes back up in its startup mode,
// instead of sitting frozen with a drive mode's LED lit.
//
// It does not stop the motors by itself: while the Teensy is hung or
// restarting, each VESC keeps its last command until its own timeout (VESC
// Tool, App Settings, General, "Timeout"; default 1000 ms) and then applies
// "Timeout Brake Current". Those two settings decide what a hang does.
//
// Register sequence follows the WDT_T4 library (tonton81) for WDT1.
namespace watchdog
{
    // WDOG1 counts in 0.5 s steps; this is its shortest setting.
    constexpr float WATCHDOG_TIMEOUT_S = 0.5f;

    // True if the last reset was caused by the watchdog. Call before begin().
    inline bool causedLastReset()
    {
        const uint32_t status = SRC_SRSR;
        SRC_SRSR = status; // flags are write-one-to-clear
        return status & SRC_SRSR_WDOG_RST_B;
    }

    inline void feed()
    {
        WDOG1_WSR = 0x5555;
        WDOG1_WSR = 0xAAAA;
    }

    inline void begin()
    {
        IOMUXC_GPR_GPR16 |= 0x200000; // needed for the watchdog reset to restart the chip cleanly
        CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);
        const uint8_t steps = static_cast<uint8_t>(WATCHDOG_TIMEOUT_S / 0.5f) - 1;
        WDOG1_WCR = WDOG_WCR_SRS | WDOG_WCR_WT(steps);
        WDOG1_WICR = static_cast<uint16_t>(1 << 14); // clear any pending interrupt flag; interrupt stays off
        WDOG1_WCR |= WDOG_WCR_WDE | WDOG_WCR_WDA | WDOG_WCR_WDT | WDOG_WCR_SRE;
        WDOG1_WMCR = 0; // disable the power-down counter
        feed();
    }
} // namespace watchdog
