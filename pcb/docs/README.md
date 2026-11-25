# LEAN_Controller - active KiCad project

Revision M VESC switch connectors, October 19, 2026. Continue editing this one project in `C:/Users/mason/OneDrive/Desktop/NEW_PCB_LEAN/Untitled`. Open **LEAN_Controller.kicad_pro** in KiCad's project manager, then launch its schematic and PCB editors. Previous states are ZIPs in `LEAN_Controller-backups`.

The master PCB contains two separate physical boards: the existing **100 x 95 mm main controller** and a **178.07 x 44.72 mm control panel**. They share a project and a five-sheet schematic. They connect by the documented harnesses, not by copper between the outlines. Fabrication exports are separated by board.

## Current design

All VESC cables originate on the main PCB. J23 receives the panel switch signal and ground from rear J17. Two 8-pin JST-PH COMM headers replace the former two-pin distribution header: **J24 (primary) pin 3=GND, pin 8=POWER SWITCH; J25 (secondary) pin 8=POWER SWITCH only. All other positions are NC.** The switch inputs remain joined because one switch controls both VESCs. Do not add a secondary signal-ground lead. The project has 95 footprints: 71 main and 24 panel. The five panel harnesses are unchanged. See POWER_SWITCH.md for cable construction and latching-switch limitations.

The original 48 main-board component positions are preserved, including the Teensy, both ICs, resistor/capacitor networks, VESC connectors and J6-J9 sensor row. The main board has 16 additions for panel I/O and four Bluetooth-interface positions (J22, C17 and optional DNP R65/R66). Eight previously unused Teensy pins now handle three mode buttons and five LEDs. Secondary VESC J1 now uses the same **four-pin B4B-XH-A / XHP-4 connector as J2**: **1=NC, 2=CANH, 3=CANL, 4=NC**. Leave cable cavities 1/4 empty. The two middle signal positions match J2 and the VESC CAN port signal order. Primary VESC J2 supplies 5 V and ground.

The panel holds both A10403RNCQ rotary switches and five indicator LEDs. Four user-selected **Dirvcp prewired momentary buttons** mount in the enclosure and plug into rear headers J18-J21. The pictured buttons are not PCB-solder parts. The owned Gebildet illuminated latching power switch also mounts in the enclosure. Five 16 mm openings in the PCB provide clearance for the button bodies; they are **not the enclosure's M12 mounting holes**.

All ten panel connectors mount on the **back**. Main-board connectors retain their existing orientation. In a front view, through-hole pads remain visible even though the plastic connector bodies and cables are behind the board. Use `panel-rear-preview.png` and `panel-rear-assembly.png` for the view from the back.

The five indicators are driven directly from Teensy GPIO27-31 through **R101-R105, 1 kohm each, on the main board before the cable**. LED cathodes return to panel ground. No LED driver IC is fitted. The existing power-switch LED receives **5 V directly** from the primary VESC logic supply through J10/J14 and J16. Its advertised rating is 12-24 V, so current and brightness at 5 V still require a bench check. U101, U102, F101 and the boost-only bulk capacitor C106 have been removed; C107 remains as local 5 V bypass.

All front control centers remain unchanged and match **Control-BoardV3.PDF**: ACCEL left / SPEED right and power in the upper row. The panel is now 178.07 x 44.72 mm, providing 0.50 mm nominal clearance on each side of the 179.07 x 45.72 mm rear opening. Six 3.2 mm NPTH mounting holes follow the casing bosses. Rear J11/J12/J13/J14/J16/J17 and C107 moved inward; panel routes and ground fills were rebuilt. The separate 6.35 mm auxiliary faceplate hole lies outside the rear cavity and is no longer cut into the PCB.

## Previous routing cleanup (revision I)

The main-board routing was rebuilt with shorter branches and fewer layer changes. Five LED resistors R101-R105 moved from the top-right corner into the lower control section. At that revision, all 304 pad assignments, component values and DNP selections match the previous version; all other 85 footprint positions are unchanged. The 100 x 95 mm outline and all connector positions are retained. See `ROUTING_CLEANUP.md`, `routing-comparison.png` and `routing-metrics.json` for the comparison and backup. Revision J subsequently changed J17 pin 2 and its local panel routing; all panel positions and holes remain unchanged.

## Bluetooth addition

J22 is the new 6-pin PH cable connector on the main board. Pins2-5 provide 5 V, GND and 3.3 V Serial1 signals. Pins1/6 reserve EN/STATE through unpopulated R65/R66 to GPIO2/32. The user's four-pin module does not yet have verified EN/STATE functionality. See `BLUETOOTH_WIRING.md` for pin-by-pin wiring and future options.

## Wiring and mechanical files

- `CONTROL_PANEL_WIRING.md`: all panel harnesses, button and LED functions, and Teensy GPIO assignments.
- `POWER_SWITCH.md`: owned latching switch, direct 5 V LED supply, and the VESC configuration assumptions still requiring hardware verification.
- `PANEL_MECHANICAL.md` and `panel-mechanical-coordinates.csv`: outline, control centers, holes and enclosure fit notes.
- `BOM.csv`: fitted board parts, DNP/conditional parts and placement side.
- `connector-parts.csv`, `connector-pinout.csv`, `off-board-parts.csv`: headers, cable-end parts and every connector pin.
- `ROTARY_WIRING.md`: retained resistor ladder values and nominal ADC decoding. The panel now performs the switch-to-header wiring on copper.

The current mechanical source is `references/Control-BoardV3.PDF`. The PCB boundary and six mounting centers match its rear cavity with 0.50 mm edge clearance. Direct checking against `references/Control-BoardV3-3DPrint.STL` found no casing intersections with the bare PCB slab, ten rear header model envelopes or C107. Check the assembled depth, standoff lengths, connector plug clearance and the actual printed enclosure before fabrication. The earlier `analog-paths` and `rotary-wiring` illustrations show the unchanged main-board ADC paths, but do not depict the new panel.

## Electrical checks and layout

Combined master: **0 DRC violations, 0 unconnected items, 0 schematic parity issues**. All five schematic sheets: **0 ERC violations**. The two extracted fabrication boards also pass DRC independently. Revision M checks all 325 pad assignments and preserves all 309 pads outside J24. Every old footprint origin except J24, all panel routes and both outlines are unchanged. Three local power-switch segments were replaced with seven 0.40 mm segments. All unused J1/J24/J25 pads are isolated. The overlapping unfilled user-added front zone was removed; the four original ground zones were refilled. Reports are `DRC.json`, `ERC.json`, `main-DRC.json` and `panel-DRC.json`; the independent pad/net audit is `design-check.json`.

Main-board signal tracks are 0.40 mm, with 0.30 mm Bluetooth and mode-button input routes. Main 5 V / 3.3 V distribution is 0.80 mm. Panel power is 0.80 mm and panel signals are 0.40 mm; ground connections use the refilled copper zones. C3 pad2 uses a solid ground-zone connection to avoid an isolated thermal spoke; other pad connection settings are retained. Minimum copper clearance is 0.25 mm; zones use 0.30 mm. Standard vias are 0.80 mm diameter / 0.40 mm drill. Both physical boards have filled ground zones on both copper layers. **Hide fill display to see traces; do not delete zones.** DRC verifies connectivity beyond the per-pad audit in `ground-connections.csv`.

These are layout and schematic checks. Firmware, cable noise immunity, powered operation and complete assembled enclosure fit have not been validated. The bare PCB and rear-header envelopes have been checked against the provided casing STL. No firmware was supplied. The controller must debounce switches, decode valid ADC ranges, handle broken/shorted cables and stop safely on faults before wheelchair use. The Disarm button is a software input, not an independent emergency power disconnect.

## Prototype fabrication

Use **two separate bare-board orders/quote items**:

| File | Physical board | Size |
|---|---|---|
| `LEAN_Controller-fabrication.zip` | Under-seat controller | 100 x 95 mm |
| `LEAN_Control_Panel-fabrication.zip` | User control panel | 178.07 x 44.72 mm |

Each ZIP contains its board's filled copper, masks, both silkscreens, Edge.Cuts outline/cutouts, plated and nonplated drill data and Gerber job file. The unpacked files are in `manufacturing/main` and `manufacturing/panel`. Do not upload the combined master as a single production PCB. Native manufacturing hashes are in `manufacturing-sha256.json`.

Prototype settings: 2 layers, FR-4, 1.6 mm thickness, 1 oz copper, green solder mask, white silkscreen, lead-free HASL, standard through vias, electrical test, no controlled impedance. The files are for prototype fabrication after checking physical parts and enclosure clearances; no order has been placed.

R37 remains **DNP**. Fit R36 (120 ohm) only if this board needs to provide one of the CAN bus's two endpoint terminations. The board powers controller electronics and sensors only. Battery and motor current remain external. Check Teensy VIN/VUSB isolation before combining USB and external 5 V; this carrier has no USB supply isolation.

## References

- [Kingbright WP7113LID](https://www.kingbrightusa.com/images/catalog/SPEC/WP7113LID.pdf)
- [JST PH catalog](https://www.jst-mfg.com/product/pdf/eng/ePH.pdf) and [JST XH series](https://www.jst-mfg.com/product/index.php?lang=2&series=277)
- [PJRC Teensy 4.1](https://www.pjrc.com/store/teensy41.html)

The user's Littelfuse rotary-switch datasheet remains in `references`. The old main-only STL is archived in the pre-change backup; use the current KiCad design and assembly drawings for dimensions.

## Separate 3D export

`3D-export/LEAN_Main_3D.kicad_pcb` and `3D-export/LEAN_Panel_3D.kicad_pcb` are one-board snapshots for the user to export STL/STEP. Both snapshots are refreshed for revision M; the main snapshot includes the two 8-pin COMM headers. Panel geometry is unchanged from revision K. The panel retains the user-selected translation (+13, -82.3) mm, preserving the export coordinates of its controls. Its reduced boundary now begins at (42.74, 38.74) mm. Existing STL files are untouched and must be re-exported to show the new outline. See `3D-export/README.md` for menu steps and missing custom-body models; refresh these snapshots after future layout changes.

Revision L backup: `LEAN_Controller-backups/before-J1-four-pin-20261019.zip`. Replace any previously uploaded main-board Gerber ZIP with the current `LEAN_Controller-fabrication.zip` to order the four-pin version. The panel electrical and mechanical design is unchanged.

Revision M backup: `LEAN_Controller-backups/before-power-COMM-20261019.zip`. Use the refreshed main fabrication ZIP; previous main exports contain the old J24. Copper fill display is enabled in the project-local settings.

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
