# Enclosure

A 3D-printed box for a master set: the C5 + C6 carrier, its speaker, a LiPo cut-off and a
DC-DC converter, with the antenna on an arm outside.

![enclosure](enclosure.png)

**Nothing has been printed yet.** The parts fit together in the model, with no interference
and no support needed, but several dimensions come from a photo or are assumed (see
[Before printing](#before-printing)).

## Files

| File | What |
| --- | --- |
| [`enclosure.sc`](enclosure.sc) | the source: a [SolidCraft](https://github.com/tangency-labs/solidcraft) 0.41 design where every dimension is a named parameter with a comment |
| [`print/`](print/) | the parts to print, as 3MF, each already laid on its print face |

After a change, export each printed body again with "lay on print faces": their print faces
are set in the design. The envelopes of the modules, the antenna and its SMA jack are
references, not parts.

## Layout

Inside 90 × 103 × 64 mm, outside 95 × 108 × 70 mm with the lid, without the arm.

- **Plates:** three of them slide down rails, front to back:
  1. the cut-off, its display and buttons facing the front window;
  2. the DC-DC converter;
  3. the master carrier, its USB-C at the top, reachable with the lid off.

  Wires pass from one plate to the next through the notches at their top.
- **Walls:**
  - front: a recess brings the window to 1 mm from the display, and two printed pushers
    reach the cut-off's buttons;
  - left: the XT60E-M for the LiPo;
  - right: the SMA bulkhead under the master carrier, and the dial of the antenna arm;
  - back: the speaker in its frame (28 × 31 mm, 15 mm deep), behind a grille.
- **Vents:** grilles low and high, on both sides of the DC-DC and master compartments.
- **Lid:** 4 M3 screws into inserts; it also holds the plates down.

## Antenna arm

- **Positions:** the arm turns about an axis square to the right wall and locks in 8
  positions 45° apart (up, down, front, back and the diagonals). Loosen its M3 screw, turn it
  a notch, tighten it.
- **Clamps:** two jaws hold the antenna by its ground band, 5.5 to 15 mm from the SMA edge,
  and stay clear of the line, the PI and the balun.
- **Pointing down:** the antenna then reaches 36 mm below the bottom of the box, so raise the box.
- **Coax:** up to 150 mm from the bulkhead to the antenna. Its plug on the wall must stand
  under 20 mm, or the antenna hits it when pointing back. A right-angle plug clears it.
- **Firmware:** build the detector with `CONFIG_CHECKPOINT_ANTENNA_IPEX=y`, then calibrate
  again with every VTX off. Without it, the C5 listens on its onboard antenna, inside the
  box.

## Printing

PETG, 3 perimeters (for the inserts), 20 to 30 % infill, no supports. The longest bridges are
about 25 mm, over the recess and the speaker grille. Print one plate first and try an insert
in its 4.2 mm holes (`insert_d`).

## Assembly

1. Set the heat-set inserts (16 M3x5x5, 1 M3x6x5 in the dial post).
2. Screw the dial post on from inside the box (2 M3 x 8), before plate 1 goes in.
3. Fit the XT60E-M (2 M2.5 screws and nuts), the SMA bulkhead and the speaker.
4. Screw each module on its plate. Put the two pushers in their holes, then slide plate 1 in,
   then plates 2 and 3.
5. Wire through the plate notches, then screw the lid on (4 M3 x 8).
6. Clamp the antenna (2 M3 x 18, nuts in the arm), fit the arm (M3 x 10 and a washer).

The parts and screws are listed in [`../BOM.csv`](../BOM.csv), rows `enclosure` and `power`.

## Before printing

Measure these on the real parts and change the parameter if it differs:

| Parameter | Now | What |
| --- | --- | --- |
| `disp_t`, `btn_t` | 8, 5 mm | assumed: height of the cut-off's display and buttons above its board |
| `disp_x0`, `disp_z0`, `disp_w`, `disp_h`, `btn1_x`, `btn2_x`, `btn_z` | from a photo | cut-off display and buttons, about 1 mm |
| `relay_z0` | 25.9 mm | from a photo: the relay slides past the recess, `relay_clear` must stay above 0 |
| XT60E-M flange | not drawn | its size, to check it misses the lid column and plate 1's rail |
| `cutoff_hole_d`, `dcdc_hole_d` | 3 mm | assumed mounting holes of the cut-off and DC-DC |
