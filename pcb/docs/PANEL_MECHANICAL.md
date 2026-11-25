# Control-panel mechanical handoff — revision K

The panel PCB is **178.07 x 44.72 mm**, nominally 1.6 mm thick. It fits within the **179.07 x 45.72 mm** rear opening in `references/Control-BoardV3.PDF`, with **0.50 mm nominal clearance on each edge**. Main-board geometry is unchanged.

## Datums and controls

Coordinates in `panel-mechanical-coordinates.csv` are millimetres from the **new top-left PCB corner, viewed from the user/front**, X right and Y down. For comparison to V3, add 0.50 mm to both coordinates to obtain coordinates from the top-left rear-cavity corner. These are different from the drawing's wider front-face datum.

Master PCB new top-left: (29.74,121.04) mm. Cavity datum: (29.24,120.54) mm. The old PCB boundary was (14,118) to (206,178) mm. All existing button, LED and rotary-shaft centers are unchanged in master coordinates; ACCEL remains left and SPEED right.

Five 16 mm PCB cutouts clear the M12 button bodies; the buttons mount to the enclosure, not these PCB openings. The old 6.35 mm auxiliary through-hole is outside the V3 rear cavity and has been removed from the PCB. It remains an enclosure feature in the drawing.

## Six casing mounts

Use six **3.2 mm NPTH holes**, H101–H106, for M3 mounting hardware. The V3 broken view gives these centers relative to the rear-cavity top-left:

| Hole | X mm | Y mm |
|---|---:|---:|
| H101, upper left | 3.00 | 3.00 |
| H102, upper middle | 70.49 | 4.00 |
| H103, upper right | 175.07 | 4.00 |
| H104, lower left | 4.00 | 41.72 |
| H105, lower middle | 70.49 | 41.72 |
| H106, lower right | 175.07 | 41.72 |

The upper-left 3 mm offsets are intentionally different from the other outer mounts' 4 mm offsets. The drawing's 70.49 + 108.59 mm dimensions differ by 0.01 mm from the 179.07 mm overall dimension; the middle center uses 70.49 mm and the right mounts use the overall width minus 4.00 mm. Hole copper exclusion is 3.2 mm radius. Use insulating spacers/washers as needed; the upper-left hole is close to the board edge and the power-button opening.

## Rear connectors and assembly depth

All ten panel connectors remain on B.Cu, with bodies and cable exits behind the panel. J11/J12 move into the upper gap above the rotary switches; J13/J14, J16/J17 and C107 move inside the smaller outline. Their electrical functions and pinouts are unchanged. J18–J21 remain behind the wired buttons. Five straight-through interboard harnesses still connect to the main board; every VESC cable originates on the main PCB.

Both A10403RNCQ rotary switches retain their existing positions and 31.75 mm shaft spacing. Their body models are not included in the custom KiCad footprint. This revision verifies the PCB plan view and mounting pattern, not a fully assembled 3D fit. Check actual rotary bodies, switch nuts, button wiring, LED lead heights, connector plugs and standoff height against the enclosure CAD or a 1:1 print before ordering.

`3D-export/LEAN_Panel_3D.kicad_pcb` is regenerated with the existing (+13,-82.3) mm translation from the master. Control export coordinates are preserved; its new outline top-left is (42.74,38.74) mm. Existing user STL files are intentionally unchanged and are now stale for the panel outline; export a new STL from this snapshot.

## STL fit check (2026-10-14)

Checked `references/Control-BoardV3-3DPrint.STL` directly as a triangle mesh. Its six boss mounting faces are at STL Z=11.8364 mm. A 1.6 mm bare PCB occupies Z=10.2364 through 11.8364 mm, with its front facing the controls. Transform from the master: STL X = PCB X - 14; STL Y = 168.8 - PCB Y.

Clipping the casing mesh against the PCB's entire rectangular slab found no intersections, allowing 0.001 mm at the intended boss-face contact. Wall clearance is 0.500 mm on every edge. All six mounting centers agree within 0.006 mm; the two middle centers differ by only the PDF's 0.005 mm rounding. The casing holes are nominally 3.2 mm.

KiCad STEP models were substituted for the library VRML references during this internal fit check. The bounding boxes of all ten rear headers and C107 have no casing intersections. Mating plugs and wire bends are not represented. The custom A10403RNCQ bodies are absent, so this does not certify the complete switch/button assembly; check their seated height and bushing engagement. Set LED lead heights to suit the faceplate. The existing user STL is untouched.

See `panel-case-fit.png`, `panel-case-fit.json` and `panel-component-envelopes.json` for the overlay and recorded geometry checks.
