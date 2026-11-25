# Bluetooth connector J22

Revision H adds a **6-pin JST-PH B6B-PH-K-S** header on the main controller's left edge. Use a **PHR-6** cable housing. The module stays off-board and connects by cable; this is not a direct module socket. The existing sensor connectors, VESC connectors and panel functions are unchanged.

## Wire by printed module labels

| J22 pin | HC-06 / ZS-040 label | Main-board connection | Populate now |
|---:|---|---|---|
| 1 | EN | GPIO2 / U7 pad4 through optional R65 (1k, DNP) | No |
| 2 | VCC | 5 V | Yes |
| 3 | GND | Ground | Yes |
| 4 | TXD | Teensy GPIO0 / Serial1 RX / U7 pad2 | Yes |
| 5 | RXD | Teensy GPIO1 / Serial1 TX / U7 pad3 | Yes |
| 6 | STATE | GPIO32 / U7 pad24 through optional R66 (1k, DNP) | No |

Pin 1 is square on the PCB. The table follows the printed module labels starting at EN; do not infer cable order from a front/back photograph. The module's **TXD output goes to the Teensy's RX**, and module **RXD receives the Teensy's TX**. The four populated module pins connect to J22 pins2-5. Match the module-end female connector to its actual header. Do not force the 2.0 mm JST housing onto the module header.

The supplied photo explicitly marks **Power: 3.6 V-6 V** and **Level: 3.3 V**. This carrier supplies VCC with 5 V and uses the Teensy's 3.3 V UART directly. Do not add the Arduino Uno guide's 5 V-to-3.3 V divider here, and do not apply 5 V to RXD, TXD, EN, STATE or Teensy GPIOs. C17 is a 100 nF bypass at J22; the remote module retains its onboard supply components.

## Optional EN and STATE

The photo shows four fitted header pins and two empty pads labeled EN and STATE. Shared ZS-040 silkscreen does **not** establish that those pads work on every HC-06 variant. Their connections, behavior and voltage must be checked on this exact module before use.

R65 and R66 are **do not populate** now, so J22 pins1/6 are isolated from the reserved Teensy GPIOs. Leave these cable positions empty and GPIO2/32 as inputs with no pull. Future use can fit the 1 kohm resistors after checking the module pin functions and 3.3 V compatibility. EN must not be treated as a verified power switch or emergency stop; on some variants its role is different or unsupported. No additional driver chip is added.

## Firmware and physical placement

Use **Serial1**, which uses GPIO0 (receive) and GPIO1 (transmit) on Teensy 4.1. USB `Serial` is separate. Set the baud rate to the module's actual configuration; 9600 is common but has not been established on this unit. No firmware was supplied or modified, and pairing/data transfer has not been bench tested.

Keep the external Bluetooth antenna clear of metal and away from motor/phase wiring. This is a local 3.3 V UART connection, not a noise-hardened long-distance link. Firmware must reject invalid commands and handle loss of communication; adding a connector does not validate wireless wheelchair operation.

## Added assembly items

- J22: one B6B-PH-K-S header and PHR-6 cable housing; four PH crimp contacts for the present module, two more only if verified optional signals are later used.
- C17: one 100 nF ceramic capacitor, at least 16 V, 5 mm lead spacing; use matching lab stock.
- R65/R66: two optional 1k axial positions, 7.62 mm lead spacing; **leave empty now**.
- Reuse the owned module and suitable module-end female leads/socket.

Sources: [PJRC hardware UART pin table](https://www.pjrc.com/teensy/td_uart.html), [Teensy 4.1 electrical limits](https://www.pjrc.com/store/teensy41.html), [first-hand comparison of ZS-040 variants](https://www.martyncurrey.com/hc-05-and-hc-06-zs-040-bluetooth-modules-first-look/), and the user's photo of the actual board. The latter comparison describes examined specimens; it is not verification of this user's EN/STATE behavior.
