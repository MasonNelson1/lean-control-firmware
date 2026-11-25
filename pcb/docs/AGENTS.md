# LEAN working KiCad project

The user wants one normal project edited in place. The canonical project is:
`NEW_PCB_LEAN/Untitled/LEAN_Controller.kicad_pro` in Mason's local KiCad folder (this `pcb/` folder is a trimmed copy).

- Make future design changes in this same project folder. Do not create revision-named project folders or ask the user to open individual design files inside ZIPs.
- Keep matching `.kicad_pro`, `.kicad_sch` and `.kicad_pcb` names. Open through KiCad's project manager so editor navigation and cross-probing work.
- Preserve prior states as backup ZIPs in `LEAN_Controller-backups`, not separate working projects.
- The board has remote A10403RNCQ switches: J4.1 → A0/GPIO14/U7 pad36, J3.1 → A1/GPIO15/U7 pad37. Connector pins2–5 → switch terminals1–4. Switch terminal9 is isolated support only.
- Keep the four sensor connectors J6–J9 together along the right edge, ordered FSR1–FSR4. Maintain the user's preference for orderly, easy-to-follow routes.
- Keep the required GND copper zones. Use display settings to hide fills rather than deleting zones.
- After electrical/layout changes, run relevant KiCad ERC/DRC with schematic parity, refill zones, and update the fabrication output. Firmware has not been provided or validated.


## Control panel revision G prototype (2026-10-11)
- The same master PCB now contains separate 100 x 95 mm main and 192 x 60 mm panel outlines. Separate fabrication ZIPs are required; do not order the combined master outline as a single board.
- Panel nets use CP_ names; four documented straight-through harnesses connect the physical boards. Keep hierarchical schematic UUID/footprint links intact.
- J11/J12/J13/J14/J16/J17/J18/J19/J20/J21 are on B.Cu (rear). Main connectors remain in their existing positions.
- SW103-SW106 are user-selected Dirvcp prewired enclosure buttons, excluded from PCB. J18-J21 are their rear 2-pin headers, signal then GND. Five 16 mm panel PCB openings provide button-body clearance, not enclosure mounting holes. Control centers match Control-BoardV2.PDF exactly; ACCEL left, SPEED right. Auxiliary 6.35 mm through-hole is included at (9.652,25.4) from panel top-left.
- SW101/SW102 are soldered A10403RNCQ switches. Five indicators are driven directly by Teensy GPIO27-31 through main-board R101-R105 (1k). U101/U102/F101/C106 removed. Owned Gebildet power-button LED receives 5 V directly; see POWER_SWITCH.md for unverified VESC configuration assumptions.
- J1 secondary VESC is CANH/CANL only; pad3 NC. Primary J2 provides logic supply/ground. Do not connect both VESC 5 V outputs or add a secondary ground lead without rechecking the system topology.
- Keep README.md, CONTROL_PANEL_WIRING.md, PANEL_MECHANICAL.md, BOM/connector CSVs and both fab packages current. Firmware and physical enclosure fit are not yet verified.

## 3D export snapshots
- The user wants the combined editable board retained if separate 3D export is possible. `3D-export/LEAN_Main_3D.kicad_pcb` and `LEAN_Panel_3D.kicad_pcb` are derived export-only copies; regenerate after design changes. The user will export STL themselves. Do not edit these as independent working projects.
- Current user canvas placement is main top-left (14.5,14) and panel (14,118) mm. Preserve user placement. Old workspace export scripts using x > 160 no longer classify the master correctly; classify by physical board bounds or footprint UUIDs.
- Panel custom parts SW101/SW102 currently lack 3D body models; do not claim complete assembled mechanical fit from an STL.

## Bluetooth revision H (2026-10-12)
- J22 on the main board is JST-PH 6-pin, pin1 EN, 2=5V, 3=GND, 4=module TXD to U7 pad2/GPIO0/Serial1 RX, 5=module RXD from U7 pad3/GPIO1/Serial1 TX, 6=STATE.
- C17 is 100nF bypass. Optional R65/R66 (1k) are DNP, isolating EN/STATE from GPIO2/U7 pad4 and GPIO32/U7 pad24 until the exact module is verified. Keep optional cable cavities1/6 empty for now. The user's photo shows only four fitted pins; shared ZS-040 labels do not prove EN/STATE functionality.
- New fifth sheet LEAN_Bluetooth.kicad_sch and BLUETOOTH_WIRING.md describe this interface. No firmware or Bluetooth operation has been tested.

## Routing cleanup revision I (2026-10-12)
- Main routing rebuilt; R101-R105 relocated into the lower control section. All pad nets, values and DNP flags unchanged; all other footprint positions, all connectors and both board outlines preserved.
- Main signals 0.40 mm; Bluetooth and mode-button input routes 0.30 mm; main power 0.80 mm. C3.2 uses a solid GND zone connection. Retain filled zones.
- Panel geometry/routing and the user-translated panel 3D export were preserved. See ROUTING_CLEANUP.md and routing-metrics.json; the previous saved project is in the before-routing-cleanup ZIP in LEAN_Controller-backups.
- Revision I passed ERC, combined DRC/parity, both fabrication-board DRC checks and an independent 304-pad net audit on 2026-10-12.

## Main-board VESC wiring revision J (2026-10-13)
- All VESC cables originate on the main board: J1/J2 plus J24 power-switch distribution.
- Panel J17 is now a straight-through two-wire cable to main J23: pin1 signal, pin2 ground. J17.2 changed from CP_PWR_SW to CP_GND. Do not reconnect the old split VESC harness to J17.
- Main J24 pins1/2 are both VESC_PWR_SW, going to the respective VESC power-switch inputs. Neither is a supply or ground. Existing latching-switch hardware/firmware assumptions remain unverified.
- Added main J23/J24 PH 2-pin headers; 92 footprints and 308 pad assignments. All 90 previous footprint positions and both outlines preserved.
- Both 3D snapshots refreshed; preserve the panel snapshot origin (27,35.7) mm, translated (+13,-82.3) from master. Keep the user's STL untouched.

## Panel casing fit revision K (2026-10-14)
- Current panel outline is 178.07 x 44.72 mm, at master top-left (29.74,121.04). The old 192 x 60 panel size and (14,118) corner are superseded. Main remains 100 x 95 mm at (14.5,14).
- Source: references/Control-BoardV3.PDF. Rear cavity 179.07 x 45.72 mm; 0.50 mm PCB clearance per side. Six 3.2 mm NPTH mounting holes follow V3 (including the asymmetric 3 mm upper-left offsets). See PANEL_MECHANICAL.md and coordinate CSV.
- Button, rotary and LED centers are unchanged. Five 16 mm button-body cutouts retained. Auxiliary 6.35 mm through-hole is an enclosure feature outside the rear cavity, removed from PCB.
- Rear J11/J12/J13/J14/J16/J17 and C107 repositioned; all panel routes rebuilt/refilled. Other panel components and all main-board components/routes/outlines preserved. All connector pinouts and electrical behavior unchanged.
- Master now 94 footprints (70 main, 24 panel), 310 pad assignments including six unnumbered NPTH mounting pads.
- Panel 3D export keeps translation (+13,-82.3) from master; control coordinates remain unchanged. New export outline corner is (42.74,38.74). The prior outer corner (27,35.7) is superseded by the reduced boundary. Do not overwrite the user's existing STL; it needs re-export for the new geometry.
- Update both fabrication packages and mechanical/BOM handoff together. Assembled depth and physical enclosure fit remain unverified.

- V3 STL verified: bare PCB slab at STL Z=10.2364..11.8364 mm has no casing intersection and 0.50 mm wall clearance; all six mounting centers agree within 0.006 mm. Rear-header/C107 envelopes clear the casing. Transform STL X=master X-14; STL Y=168.8-master Y. Full rotary/button/cable assembly still requires physical/CAD verification. See panel-case-fit.json.

## J1 four-pin CAN revision L (2026-10-19)
- Supersedes earlier three-pin J1 instructions. J1 is B4B-XH-A / XHP-4, matching J2's physical four-pin header. Pin1 NC, pin2 CANH, pin3 CANL, pin4 NC. Leave the two outer cable cavities empty. J2 remains 5V/CANH/CANL/GND.
- Preserve J1 origin (80.5,19) and all other footprint origins. Only six local J1 track segments were replaced with seven 0.40 mm segments. All panel routes and both outlines are unchanged.
- Master has 94 footprints and 311 pads; all 307 non-J1 pads unchanged. ERC, master DRC/parity, separate fabrication DRCs and both 3D DRCs pass. Outer J1 pads verified isolated. Both fab ZIPs and 3D snapshots refreshed.
- Backup: LEAN_Controller-backups/before-J1-four-pin-20261019.zip. Use the new root-level main fabrication ZIP for ordering; old main exports have three-pin J1.

## VESC COMM switch connectors revision M (2026-10-19)
- Replaces the old two-pin J24: J24 and new J25 are B8B-PH-K-S / PHR-8. J24.3=GND, J24.8=VESC_PWR_SW; J25.8=VESC_PWR_SW. Every other pin is NC. Only primary COMM cable cavities3/8 and secondary cavity8 are populated. Never add a secondary GND or supply wire.
- Both switch outputs share VESC_PWR_SW; the owned dry contact connects that net to GND via J16/J17/J23. Latching behavior remains unverified on the actual installed VESC firmware.
- J24 moved to (94.5,19); J25 is (94.5,35). All other footprint origins, both outlines and panel tracks preserved. Three switch tracks replaced with seven0.4mm tracks.
- Removed one duplicate, unfilled F.Cu GND zone added by the user over the original. Four original zones refilled and retained. Local zone display enabled.
- 95 footprints (71main/24panel),325pads; all309 non-J24 pads unchanged. ERC, master DRC/parity, both fab DRCs and both3D DRCs pass.15 NC pads at J1/J24/J25 verified isolated.
- Backup: LEAN_Controller-backups/before-power-COMM-20261019.zip. Both fab ZIPs, derived3D boards, wiring docs and BOMs refreshed.

## Capacitor clearance and remote-input routing revision N (2026-10-25)
- C1 and C2 moved down 1.75 mm to y=79.75 mm. Nominal capacitor-body clearance from the Teensy bottom edge is now 2.02 mm instead of 0.27 mm. Actual lab-stock body dimensions still need to match the footprint.
- R101 moved down 1.00 mm to y=83.00 mm to accommodate C1. All other component origins, connector positions, electrical pad assignments and both board outlines are unchanged.
- Rebuilt only nearby SPEED_ADC/ACCEL_ADC and LED routes around the moved components. C1 ground pad uses a solid zone connection to avoid an isolated thermal spoke. R49 reference moved for legibility.
- PANEL_REMOTE_RAW now uses one via at (85.5,99.5). The lower via at (85.5,101) was removed and the J10 connection stays on F.Cu. Its R63 branch still joins B.Cu at the retained via.
- Panel component positions and routes unchanged. Four GND zones retained/refilled. ERC, master DRC/parity, both manufacturing-board DRCs, both derived 3D DRCs and all 325 pad/net checks pass.
- Both fabrication ZIPs and 3D snapshots regenerated. No parts added or removed; BOM and cable pinouts unchanged. Prior state: LEAN_Controller-backups/before-clearance-cleanup-20261025.zip.

## Main manufacturing cleanup revision O (2026-10-25)
- Moved the existing via beside U1 pin 8 from (89.2,67.2) to (88.9,67.2) mm and adjusted its two attached trace endpoints. Drill-edge clearance increased from 0.321 to 0.621 mm. No vias or components added.
- Enlarged all 34 main-board silkscreen text items below 1 mm to 1 mm. Repositioned nearby references/legends as needed for legibility. No main text remains below 1 mm, and a separate main-only check with 0.15 mm silk clearance and 0.45 mm hole clearance passes.
- All 325 pad assignments, all footprint origins, both outlines and the panel's footprints/routes are unchanged. All four ground zones retained/refilled. ERC, master DRC/parity, both fabrication DRCs, both 3D DRCs and the additional main manufacturing check pass.
- Both fabrication packages and derived 3D snapshots refreshed. BOM, schematic and cable pinouts unchanged. Backup: LEAN_Controller-backups/before-manufacturing-cleanup-20261025.zip.
- Remaining operational limitations are unchanged: sensor calibration, VESC latching-switch configuration, CAN termination population, power-button LED brightness at 5 V and firmware behavior still require hardware validation.
