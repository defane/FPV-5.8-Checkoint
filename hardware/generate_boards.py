#!/usr/bin/env python3
"""The two checkpoint carrier boards, written as KiCad 9 files, then exported.

    python3 hardware/generate_boards.py            # needs KiCad 9 (pcbnew, kicad-cli)
    python3 hardware/generate_boards.py --no-kicad # writes the .kicad_pcb only

node:   Waveshare ESP32-C5-Zero + ESP32-C3-Zero
master: Waveshare ESP32-C5-Zero + ESP32-C6-Zero + MAX98357A breakout

This script is the source of the boards: geometry, nets and routing are all here. Why
they are the way they are is in hardware/README.md. y grows towards the antennas; the
top edge (y = 0) is the USB edge.
"""
import json
import os
import subprocess
import sys
import uuid
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORIGIN = 100.0                     # where the board sits on the KiCad sheet
F, B = "F.Cu", "B.Cu"

# Waveshare drawings, checked on their DXF: 18 mm wide, two rows of 9 pins.
PITCH, ROW, FIRST, INSET, MOD_W = 2.54, 15.24, 1.59, 1.38, 18.0
LEFT = ["5V", "GND", "3V3", "0", "1", "2", "3", "4", "5"]
MODULES = {
    "C5": {"len": 28.0, "right": ["TX", "RX", "13", "14", "10", "9", "8", "7", "6"]},
    "C6": {"len": 23.5, "right": ["TX", "RX", "14", "15", "18", "19", "20", "21", "22"]},
    "C3": {"len": 23.5, "right": ["21", "20", "19", "18", "10", "9", "8", "7", "6"]},
}
USB_OVERHANG = 1.6

# MAX98357A breakout (Adafruit's board file): row 2.54 mm from the edge, 17.78 x 19.05.
AMP_PINS = ["LRC", "BCLK", "DIN", "GAIN", "SD", "GND", "VIN"]   # top to bottom here
AMP_W, AMP_DEPTH, AMP_INSET = 17.78, 19.05, 2.54
AMP_TERMINAL = 12.95               # speaker terminal's pins, past the row

HEIGHT = 24.0                      # last pin at 21.91 + a ring + a margin
C5_X, BRIDGE_X = 13.0, 33.5        # 13 mm of power zone, a 2.5 mm gap between modules
MARGIN = 5.5                       # an M3 screw head next to a module
NOTCH_TOP, NOTCH_IN = 19.2, 2.9    # the cut under an antenna, between the pin rows
HOLE = 2.75

SIGNAL, POWER, NECK = 0.25, 0.8, 0.4   # NECK: between two pins of a row
L5, R5 = C5_X + INSET, C5_X + INSET + ROW
LB, RB = BRIDGE_X + INSET, BRIDGE_X + INSET + ROW
MID = (R5 + LB) / 2


def row(i):
    return round(FIRST + i * PITCH, 3)


GAP_2_3, GAP_6_7 = row(2) + PITCH / 2, row(6) + PITCH / 2   # lanes through a pin row


def num(v):
    return f"{round(v, 4):g}"


def xy(p):
    return f"{num(p[0] + ORIGIN)} {num(p[1] + ORIGIN)}"


def q(s):
    return '"' + s.replace('"', '\\"') + '"'


class Board:
    def __init__(self, name, width):
        self.name, self.width = name, width
        self.nets = ["", "GND"]
        self.items = []
        self.count = 0

    def id(self):
        self.count += 1
        return f'(uuid "{uuid.uuid5(uuid.NAMESPACE_URL, f"fpv58/{self.name}/{self.count}")}")'

    def net(self, name):
        if name not in self.nets:
            self.nets.append(name)
        return f"(net {self.nets.index(name)} {q(name)})"

    def path(self, net, layer, width, *pts):
        self.net(net)
        n = self.nets.index(net)
        for a, b in zip(pts, pts[1:]):
            self.items.append(f"(segment (start {xy(a)}) (end {xy(b)}) (width {num(width)}) "
                              f'(layer "{layer}") (net {n}) {self.id()})')

    def via(self, net, p, size=0.7, drill=0.3):
        self.net(net)
        self.items.append(f'(via (at {xy(p)}) (size {num(size)}) (drill {num(drill)}) (layers "F.Cu" "B.Cu") '
                          f"(net {self.nets.index(net)}) {self.id()})")

    def line(self, a, b, layer="F.SilkS", width=0.15):
        self.items.append(f"(gr_line (start {xy(a)}) (end {xy(b)}) (stroke (width {num(width)}) (type default)) "
                          f'(layer "{layer}") {self.id()})')

    def arc(self, a, mid, b, layer="Edge.Cuts", width=0.05):
        self.items.append(f"(gr_arc (start {xy(a)}) (mid {xy(mid)}) (end {xy(b)}) "
                          f'(stroke (width {num(width)}) (type default)) (layer "{layer}") {self.id()})')

    def text(self, s, p, size=1.0, layer="F.SilkS", rot=0):
        mirror = " (justify mirror)" if layer.startswith("B.") else ""
        self.items.append(f'(gr_text {q(s)} (at {xy(p)} {rot}) (layer "{layer}") {self.id()} '
                          f"(effects (font (size {num(size)} {num(size)}) (thickness {num(size * 0.15)})){mirror}))")

    def pad(self, name, p, drill=1.0, size=1.6, net=None, solid=False):
        n = f" {self.net(net)}" if net else ""
        zc = " (zone_connect 2)" if solid else ""
        return (f'(pad {q(name)} thru_hole circle (at {num(p[0])} {num(p[1])}) (size {num(size)} {num(size)}) '
                f'(drill {num(drill)}) (layers "*.Cu" "*.Mask"){n}{zc} {self.id()})')

    def smd(self, name, p, size, net):
        return (f'(pad {q(name)} smd roundrect (at {num(p[0])} {num(p[1])}) (size {num(size[0])} {num(size[1])}) '
                f'(layers "F.Cu" "F.Mask" "F.Paste") (roundrect_rratio 0.15) {self.net(net)} {self.id()})')

    def fp_line(self, a, b, layer, width=0.15):
        return (f"(fp_line (start {num(a[0])} {num(a[1])}) (end {num(b[0])} {num(b[1])}) "
                f'(stroke (width {num(width)}) (type solid)) (layer "{layer}") {self.id()})')

    def fp_rect(self, x0, y0, x1, y1, layer, width=0.1):
        corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
        return [self.fp_line(a, b, layer, width) for a, b in zip(corners, corners[1:])]

    def footprint(self, ref, value, at, body, attr="through_hole"):
        name = value.split()[0]
        props = "".join(
            f'(property {q(k)} {q(v)} (at 0 0 0) (layer "F.Fab") (hide yes) {self.id()} '
            f"(effects (font (size 0.8 0.8) (thickness 0.12))))"
            for k, v in (("Reference", ref), ("Value", value)))
        self.items.append(f'(footprint {q(name)} (layer "F.Cu") {self.id()} (at {xy(at)}) {props} '
                          f'(attr {attr}) {" ".join(body)})')

    def module(self, ref, kind, x0, nets):
        """A Zero module on two rows of pin headers, its USB end flush with the top edge."""
        spec, body = MODULES[kind], []
        for side, names in ((0, LEFT), (1, spec["right"])):
            for i, pin in enumerate(names):
                p = (INSET + side * ROW, row(i))
                body.append(self.pad(pin, p, net=nets.get(pin)))
        for x in (0, MOD_W):                                   # the pads keep clear of it
            body.append(self.fp_line((x, 0.4), (x, 16.0), "F.SilkS"))
        body += self.fp_rect(0, 0, MOD_W, spec["len"], "F.Fab")
        body += self.fp_rect(4.5, -USB_OVERHANG, MOD_W - 4.5, 7.0, "F.Fab")
        self.footprint(ref, f"{kind}-Zero", (x0, 0), body)
        self.text(f"{ref} {kind}-Zero", (x0 + MOD_W / 2, 6.0), 1.2)
        self.text("USB", (x0 + MOD_W / 2, 2.4), 1.0)

    def diode(self, ref, at, anode, cathode):
        body = [self.smd("1", (2.0, 0), (2.5, 1.8), cathode), self.smd("2", (-2.0, 0), (2.5, 1.8), anode)]
        body += [self.fp_line((-2.2, y), (2.2, y), "F.SilkS") for y in (-1.35, 1.35)]
        body.append(self.fp_line((0.5, -1.35), (0.5, 1.35), "F.SilkS", 0.3))    # the cathode bar
        body += self.fp_rect(-2.2, -1.35, 2.2, 1.35, "F.Fab")
        self.footprint(ref, "SS34 3A 40V SMA", at, body, "smd")

    def resistor(self, ref, at, a, b):
        body = [self.smd("1", (-1.0375, 0), (1.175, 1.45), a), self.smd("2", (1.0375, 0), (1.175, 1.45), b)]
        self.footprint(ref, "1k 0805", at, body, "smd")

    def screw_hole(self, ref, at):
        body = [f'(pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask") {self.id()})']
        self.footprint(ref, "M3 hole", at, body, "exclude_from_pos_files exclude_from_bom")

    def wire_pad(self, ref, at, net, label, label_y):
        body = [self.pad("1", (0, 0), drill=1.5, size=3.2, net=net, solid=True)]
        self.footprint(ref, f"{label} wire pad", at, body)
        self.text(label, (at[0], label_y), 1.0)
        self.text(label, (at[0], label_y), 1.0, "B.SilkS")

    def outline(self, notches):
        w, h, r = self.width, HEIGHT, 1.0
        k = 0.2929 * r                                        # 1 - cos 45
        self.line((r, 0), (w - r, 0), "Edge.Cuts", 0.05)
        self.arc((w - r, 0), (w - k, k), (w, r))
        self.line((w, r), (w, h - r), "Edge.Cuts", 0.05)
        self.arc((w, h - r), (w - k, h - k), (w - r, h))
        x = w - r
        n = 0.6                                               # a 1.2 mm router bit
        c = 0.2929 * n
        for a, b in sorted(notches, reverse=True):
            self.line((x, h), (b, h), "Edge.Cuts", 0.05)
            self.line((b, h), (b, NOTCH_TOP + n), "Edge.Cuts", 0.05)
            self.arc((b, NOTCH_TOP + n), (b - c, NOTCH_TOP + c), (b - n, NOTCH_TOP))
            self.line((b - n, NOTCH_TOP), (a + n, NOTCH_TOP), "Edge.Cuts", 0.05)
            self.arc((a + n, NOTCH_TOP), (a + c, NOTCH_TOP + c), (a, NOTCH_TOP + n))
            self.line((a, NOTCH_TOP + n), (a, h), "Edge.Cuts", 0.05)
            x = a
        self.line((x, h), (r, h), "Edge.Cuts", 0.05)
        self.arc((r, h), (k, h - k), (0, h - r))
        self.line((0, h - r), (0, r), "Edge.Cuts", 0.05)
        self.arc((0, r), (k, k), (r, 0))

    def ground_pour(self):
        w, h = self.width, HEIGHT
        pts = " ".join(f"(xy {xy(p)})" for p in ((0, 0), (w, 0), (w, h), (0, h)))
        self.items.append(f'(zone (net 1) (net_name "GND") (layers "F.Cu" "B.Cu") {self.id()} (hatch edge 0.5) '
                          "(connect_pads (clearance 0.25)) (min_thickness 0.2) (filled_areas_thickness no) "
                          "(fill yes (thermal_gap 0.3) (thermal_bridge_width 0.4)) "
                          f"(polygon (pts {pts})))")

    def keepout(self, x0, y0, x1, y1):
        pts = " ".join(f"(xy {xy(p)})" for p in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)))
        self.items.append(f'(zone (net 0) (net_name "") (layers "F.Cu" "B.Cu") {self.id()} (hatch edge 0.5) '
                          "(connect_pads (clearance 0)) (min_thickness 0.25) (filled_areas_thickness no) "
                          "(keepout (tracks not_allowed) (vias not_allowed) (pads not_allowed) "
                          "(copperpour not_allowed) (footprints allowed)) "
                          "(fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) "
                          f"(polygon (pts {pts})))")

    def kicad_pcb(self):
        layers = [(0, "F.Cu", "signal", None), (2, "B.Cu", "signal", None),
                  (9, "F.Adhes", "user", "F.Adhesive"), (11, "B.Adhes", "user", "B.Adhesive"),
                  (13, "F.Paste", "user", None), (15, "B.Paste", "user", None),
                  (5, "F.SilkS", "user", "F.Silkscreen"), (7, "B.SilkS", "user", "B.Silkscreen"),
                  (1, "F.Mask", "user", None), (3, "B.Mask", "user", None),
                  (17, "Dwgs.User", "user", "User.Drawings"), (19, "Cmts.User", "user", "User.Comments"),
                  (25, "Edge.Cuts", "user", None), (27, "Margin", "user", None),
                  (31, "F.CrtYd", "user", "F.Courtyard"), (29, "B.CrtYd", "user", "B.Courtyard"),
                  (35, "F.Fab", "user", None), (33, "B.Fab", "user", None)]
        lines = ['(kicad_pcb (version 20241229) (generator "fpv58") (generator_version "9.0")',
                 "(general (thickness 1.6) (legacy_teardrops no))", '(paper "A4")', "(layers"]
        lines += [f"({i} {q(n)} {t}{' ' + q(u) if u else ''})" for i, n, t, u in layers]
        lines += [")", "(setup (pad_to_mask_clearance 0) (allow_soldermask_bridges_in_footprints no))"]
        lines += [f"(net {i} {q(n)})" for i, n in enumerate(self.nets)]
        return "\n".join(lines + self.items + [")"]) + "\n"


def power_zone(b):
    """The wire pads and the two diodes: nothing reaches a module's USB VBUS but through one."""
    b.wire_pad("J1", (3.0, row(3)), "+5V", "+5V", 6.9)
    b.wire_pad("J2", (3.0, row(5)), "GND", "GND", 16.7)
    b.diode("D1", (9.5, 3.0), "+5V", "5V_C5")
    b.diode("D2", (9.5, GAP_2_3), "+5V", "5V_BRIDGE")
    b.text("D1", (9.5, 5.2), 0.8)
    b.text("D2", (9.5, 10.1), 0.8)
    b.path("+5V", F, 1.0, (3.0, row(3)), (6.23, row(3)), (7.5, GAP_2_3), (7.5, 3.0))
    b.path("5V_C5", F, POWER, (11.5, 3.0), (11.5 + 3.0 - row(0), row(0)), (L5, row(0)))
    b.path("5V_BRIDGE", F, POWER, (11.5, GAP_2_3), (13.2, GAP_2_3))
    b.via("5V_BRIDGE", (13.2, GAP_2_3), 1.0, 0.5)
    y = GAP_2_3
    b.path("5V_BRIDGE", B, NECK, (13.2, y), (15.6, y))
    b.path("5V_BRIDGE", B, POWER, (15.6, y), (28.4, y))
    b.path("5V_BRIDGE", B, NECK, (28.4, y), (30.8, y))
    b.path("5V_BRIDGE", B, POWER, (30.8, y), (MID, y), (MID, 3.2), (MID + 3.2 - row(0), row(0)), (LB, row(0)))
    for i, c in enumerate(((HOLE, HOLE), (HOLE, HEIGHT - HOLE),
                           (b.width - HOLE, HOLE), (b.width - HOLE, HEIGHT - HOLE))):
        b.screw_hole(f"H{i + 1}", c)


def back_legend(b, what, current):
    for i, line in enumerate(("FPV 5.8 Checkpoint", what)):
        b.text(line, (22.0, 11.5 + i * 1.6), 0.8, "B.SilkS")
    for i, line in enumerate((f"+5V {current}, 4.6 to 5.5 V", "D1 D2 SS34, R1 R2 1k")):
        b.text(line, (42.5, 11.5 + i * 1.6), 0.8, "B.SilkS")


def stitch(b, points):
    for p in points:
        b.via("GND", p)


def node():
    b = Board("checkpoint-node", C5_X + MOD_W + 2.5 + MOD_W + MARGIN)
    b.module("U1", "C5", C5_X, {"5V": "5V_C5", "GND": "GND", "6": "C5_TX", "8": "C5_RX"})
    b.module("U2", "C3", BRIDGE_X, {"5V": "5V_BRIDGE", "GND": "GND", "4": "BRIDGE_RX", "5": "BRIDGE_TX"})
    power_zone(b)
    # C5 TX 6 -> C3 RX 4
    y = 20.64
    b.resistor("R1", (MID, y), "C5_TX", "BRIDGE_RX")
    b.path("C5_TX", F, SIGNAL, (R5, row(8)), (R5 + 1.27, y), (MID - 1.0375, y))
    b.path("BRIDGE_RX", F, SIGNAL, (MID + 1.0375, y), (LB - 1.27, y), (LB, row(7)))
    # C3 TX 5 -> C5 RX 8, crossing it on the back
    y, v = GAP_6_7, (33.2, 16.6)
    b.resistor("R2", (MID, y), "C5_RX", "BRIDGE_TX")
    b.path("C5_RX", F, SIGNAL, (R5, row(6)), (R5 + 1.27, y), (MID - 1.0375, y))
    b.path("BRIDGE_TX", F, SIGNAL, (MID + 1.0375, y), (v[0], 17.2), v)
    b.via("BRIDGE_TX", v)
    b.path("BRIDGE_TX", B, SIGNAL, v, (v[0], row(8) - (LB - v[0])), (LB, row(8)))
    b.text("1k", (MID, 15.2), 0.8)
    stitch(b, [(5.5, 12), (11.5, 12), (11.5, 16), (17, 3), (25, 3), (17, 13), (25, 13), (33.5, row(4) + PITCH / 2),
               (37.5, 3), (45.5, 3), (37.5, 13), (45.5, 13), (54.2, 8), (54.2, 16)])
    b.text("FPV 5.8", (9.0, 13.0), 1.0)
    b.text("NODE", (9.0, 14.7), 1.0)
    b.text("C5 + C3  v1", (9.0, 16.4), 0.8)
    back_legend(b, "node C5 + C3", "1 A")
    b.outline([(C5_X + NOTCH_IN, C5_X + MOD_W - NOTCH_IN), (BRIDGE_X + NOTCH_IN, BRIDGE_X + MOD_W - NOTCH_IN)])
    b.ground_pour()
    return b


def master():
    amp = BRIDGE_X + MOD_W + 2.5 + AMP_INSET                   # the amplifier's pin row
    b = Board("checkpoint-master", amp + AMP_DEPTH - AMP_INSET + MARGIN + 0.45)
    b.module("U1", "C5", C5_X, {"5V": "5V_C5", "GND": "GND", "6": "C5_TX", "8": "C5_RX"})
    b.module("U2", "C6", BRIDGE_X, {"5V": "5V_BRIDGE", "GND": "GND", "5": "BRIDGE_RX", "4": "BRIDGE_TX",
                                    "18": "BCLK", "19": "LRC", "20": "DIN", "21": "SD"})
    # The breakout's body lies over the board, its row on the same grid as the C6's pins.
    body = [b.pad(pin, (0, row(i + 1)), net=n) for i, (pin, n) in enumerate(zip(AMP_PINS, (
        "LRC", "BCLK", "DIN", None, "SD", "GND", "5V_BRIDGE")))]
    body += b.fp_rect(-AMP_INSET, row(4) - AMP_W / 2, AMP_DEPTH - AMP_INSET, row(4) + AMP_W / 2, "F.SilkS", 0.15)
    b.footprint("U3", "MAX98357A breakout", (amp, 0), body)
    for i, pin in enumerate(AMP_PINS):
        b.text(pin, (amp + 3.2, row(i + 1)), 0.8)
    b.text("U3 MAX98357A", (amp + 10.5, row(4)), 1.0, rot=90)
    # What the speaker terminal's pins may touch under the breakout.
    b.keepout(amp + AMP_TERMINAL - 2.5, 6.5, amp + AMP_TERMINAL + 2.5, 17.0)

    power_zone(b)
    # The bridge's 5 V goes on to the amplifier's VIN, at the far end of its row.
    y = GAP_6_7
    b.path("5V_BRIDGE", B, POWER, (MID, GAP_2_3), (MID, y - 1.3), (MID + 1.3, y))
    b.path("5V_BRIDGE", B, NECK, (MID + 1.3, y), (36.1, y))
    b.path("5V_BRIDGE", B, POWER, (36.1, y), (48.9, y))
    b.path("5V_BRIDGE", B, NECK, (48.9, y), (51.4, y))
    b.path("5V_BRIDGE", B, POWER, (51.4, y), (51.4 + row(7) - y, row(7)), (amp, row(7)))
    # C5 TX 6 -> C6 RX 5
    b.resistor("R1", (MID, row(8)), "C5_TX", "BRIDGE_RX")
    b.path("C5_TX", F, SIGNAL, (R5, row(8)), (MID - 1.0375, row(8)))
    b.path("BRIDGE_RX", F, SIGNAL, (MID + 1.0375, row(8)), (LB, row(8)))
    # C6 TX 4 -> C5 RX 8
    b.resistor("R2", (MID, y), "C5_RX", "BRIDGE_TX")
    b.path("C5_RX", F, SIGNAL, (R5, row(6)), (R5 + 1.27, y), (MID - 1.0375, y))
    b.path("BRIDGE_TX", F, SIGNAL, (MID + 1.0375, y), (LB - 1.27, y), (LB, row(7)))
    b.text("1k", (MID, 16.4), 0.8)
    # I2S: only BCLK and LRC swap on the way, LRC takes the back.
    b.path("BCLK", F, SIGNAL, (RB, row(4)), (RB + 0.67, row(4)), (RB + 5.75, row(2)), (amp, row(2)))
    b.path("DIN", F, SIGNAL, (RB, row(6)), (RB + 2, row(6) - 2), (RB + 2, 13.0), (RB + 5.79, row(3)), (amp, row(3)))
    b.path("SD", F, SIGNAL, (RB, row(7)), (RB + 0.67, row(7)), (RB + 5.75, row(5)), (amp, row(5)))
    b.path("LRC", B, SIGNAL, (RB, row(5)), (RB + 1.46, row(5) - 1.46), (RB + 1.46, 9.09), (amp, row(1)))
    stitch(b, [(5.5, 12), (11.5, 12), (11.5, 16), (17, 3), (25, 3), (17, 13), (25, 13), (33.5, row(4) + PITCH / 2),
               (37.5, 3), (45.5, 3), (37.5, 13), (45.5, 13), (60, 1.4), (68, 1.4), (60, 22.3), (68, 22.3),
               (76.25, 8), (76.25, 16)])
    b.text("FPV 5.8", (9.0, 13.0), 1.0)
    b.text("MASTER", (9.0, 14.7), 1.0)
    b.text("C5 + C6  v1", (9.0, 16.4), 0.8)
    back_legend(b, "master C5 + C6", "2 A")
    b.outline([(C5_X + NOTCH_IN, C5_X + MOD_W - NOTCH_IN), (BRIDGE_X + NOTCH_IN, BRIDGE_X + MOD_W - NOTCH_IN)])
    b.ground_pour()
    return b


def kicad_pro(name):
    return json.dumps({
        "board": {"design_settings": {
            "defaults": {"board_outline_line_width": 0.05},
            "rules": {"min_clearance": 0.15, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.25,
                      "min_hole_to_hole": 0.25, "min_track_width": 0.2, "min_via_annular_width": 0.15,
                      "min_via_diameter": 0.6, "min_through_hole_diameter": 0.3, "min_text_height": 0.8,
                      "min_text_thickness": 0.08, "min_silk_clearance": 0.0}}},
        "meta": {"filename": f"{name}.kicad_pro", "version": 3},
        "net_settings": {"classes": [{"name": "Default", "clearance": 0.2, "track_width": 0.25, "via_diameter": 0.7,
                                      "via_drill": 0.3, "microvia_diameter": 0.3, "microvia_drill": 0.1,
                                      "wire_width": 6, "bus_width": 12, "line_style": 0, "schematic_color": "rgba(0,0,0,0)",
                                      "pcb_color": "rgba(0,0,0,0)", "priority": 2147483647}], "meta": {"version": 4}},
    }, indent=2) + "\n"


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def export(name):
    import pcbnew
    pcb = HERE / name / f"{name}.kicad_pcb"
    board = pcbnew.LoadBoard(str(pcb))
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(pcb))

    cli = os.environ.get("KICAD_CLI", "kicad-cli")
    report = HERE / name / "drc.json"
    run(cli, "pcb", "drc", "--format", "json", "--severity-all", "-o", str(report), str(pcb))
    found = json.loads(report.read_text())
    report.unlink()
    print(name, "violations", [(v["severity"], v["type"], v["description"]) for v in found["violations"]],
          "unconnected", len(found["unconnected_items"]))

    out = HERE / "gerbers" / name
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():
        old.unlink()
    run(cli, "pcb", "export", "gerbers", "-o", f"{out}/", "--layers",
        "F.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts", "--subtract-soldermask", "--no-x2", "--no-netlist",
        str(pcb))
    run(cli, "pcb", "export", "drill", "-o", f"{out}/", "--format", "excellon", "--excellon-units", "mm",
        "--excellon-separate-th", str(pcb))
    with zipfile.ZipFile(HERE / "gerbers" / f"{name}-gerbers.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(out.iterdir()):
            if f.suffix != ".gbrjob":
                z.write(f, f.name)
            f.unlink()
    out.rmdir()
    # Drawn on the fab layer: the modules' outlines, to lay the real ones on a 1:1 print.
    run(cli, "pcb", "export", "pdf", "-o", str(HERE / f"{name}/{name}-1to1.pdf"), "--layers",
        "F.Fab,F.SilkS,Edge.Cuts", "--mode-single", "--black-and-white", "--sketch-pads-on-fab-layers", str(pcb))
    svg = HERE / f"{name}/{name}.svg"
    run(cli, "pcb", "export", "svg", "-o", str(svg), "--layers", "F.Fab,F.SilkS,Edge.Cuts", "--mode-single",
        "--fit-page-to-board", "--exclude-drawing-sheet", "--sketch-pads-on-fab-layers", "--black-and-white", str(pcb))
    # Readable on a dark page too.
    svg.write_text(svg.read_text().replace("</title>", '</title>\n<rect width="100%" height="100%" fill="white"/>', 1))


def main():
    for b in (node(), master()):
        folder = HERE / b.name
        folder.mkdir(exist_ok=True)
        (folder / f"{b.name}.kicad_pcb").write_text(b.kicad_pcb())
        (folder / f"{b.name}.kicad_pro").write_text(kicad_pro(b.name))
        print(b.name, f"{b.width:g} x {HEIGHT:g} mm")
        if "--no-kicad" not in sys.argv:
            export(b.name)


if __name__ == "__main__":
    main()
