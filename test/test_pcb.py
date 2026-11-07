#!/usr/bin/env python3
"""The carrier boards stay in step with the firmware: the pins come from its Kconfig
defaults, the nets and the pad positions from the .kicad_pcb files."""
import csv
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ORIGIN = 100.0


def sexpr(text):
    stack, node = [], []
    for tok in re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text):
        if tok == "(":
            stack.append(node)
            node = []
        elif tok == ")":
            stack[-1].append(node)
            node = stack.pop()
        else:
            node.append(tok.strip('"'))
    return node[0]


def find(node, key):
    return [n for n in node if isinstance(n, list) and n and n[0] == key]


def load(name):
    """{(reference, pad): (net or None, x, y)} of one board."""
    board = sexpr((ROOT / f"hardware/{name}/{name}.kicad_pcb").read_text())
    pads = {}
    for fp in find(board, "footprint"):
        ref = next(p[2] for p in find(fp, "property") if p[1] == "Reference")
        fx, fy = (float(v) for v in find(fp, "at")[0][1:3])
        for pad in find(fp, "pad"):
            net = find(pad, "net")
            x, y = (float(v) for v in find(pad, "at")[0][1:3])
            pads[ref, pad[1]] = (net[0][2] if net else None, fx + x - ORIGIN, fy + y - ORIGIN)
    return pads


def default(path, name):
    text = (ROOT / path).read_text()
    return str(re.search(rf"config {name}\b.*?default (\d+)", text, re.S).group(1))


C5_TX = default("components/checkpoint/Kconfig", "CHECKPOINT_LINK_TX_GPIO")
C5_RX = default("components/checkpoint/Kconfig", "CHECKPOINT_LINK_RX_GPIO")
BRIDGES = {
    "checkpoint-node": (default("firmware/node/main/Kconfig.projbuild", "C3_UART_TX_GPIO"),
                        default("firmware/node/main/Kconfig.projbuild", "C3_UART_RX_GPIO")),
    "checkpoint-master": (default("firmware/master/main/Kconfig.projbuild", "C6_UART_TX_GPIO"),
                          default("firmware/master/main/Kconfig.projbuild", "C6_UART_RX_GPIO")),
}
IQ_PINS = ["0", "1", "2", "3", "4", "5"]      # the receiver's, CLAUDE.md "GPIO map"


class Boards(unittest.TestCase):
    def setUp(self):
        self.boards = {name: load(name) for name in BRIDGES}

    def assert_through_a_resistor(self, pads, a, b):
        ends = {pads[a][0], pads[b][0]}
        for ref in ("R1", "R2"):
            if {pads[ref, "1"][0], pads[ref, "2"][0]} == ends:
                return
        self.fail(f"no resistor between {a} and {b}")

    def test_the_link_is_crossed_through_a_resistor(self):
        for name, (tx, rx) in BRIDGES.items():
            pads = self.boards[name]
            self.assert_through_a_resistor(pads, ("U1", C5_TX), ("U2", rx))
            self.assert_through_a_resistor(pads, ("U1", C5_RX), ("U2", tx))

    def test_amplifier(self):
        pads = self.boards["checkpoint-master"]
        kconfig = "firmware/master/main/Kconfig.projbuild"
        for pin, option in (("BCLK", "BCLK"), ("LRC", "WS"), ("DIN", "DOUT"), ("SD", "SD")):
            gpio = default(kconfig, f"C6_AUDIO_{option}_GPIO")
            self.assertEqual(pads["U3", pin][0], pads["U2", gpio][0], pin)
            self.assertIsNotNone(pads["U3", pin][0], pin)
        self.assertEqual(pads["U3", "GND"][0], "GND")
        self.assertEqual(pads["U3", "VIN"][0], pads["U2", "5V"][0], "VIN follows the bridge's diode")
        self.assertIsNone(pads["U3", "GAIN"][0])

    def test_nothing_reaches_a_usb_vbus_but_through_a_diode(self):
        for name, pads in self.boards.items():
            rail = pads["J1", "1"][0]
            for module, diode in (("U1", "D1"), ("U2", "D2")):
                self.assertEqual(pads[diode, "2"][0], rail, "anode on the pads")
                self.assertEqual(pads[diode, "1"][0], pads[module, "5V"][0], "cathode on the module")
                self.assertNotEqual(pads[module, "5V"][0], rail)
            self.assertNotEqual(pads["U1", "5V"][0], pads["U2", "5V"][0], "each USB alone on its side")
            for ref in ("J2", "U1", "U2"):
                self.assertEqual(pads[ref, "GND" if ref != "J2" else "1"][0], "GND", name)

    def test_the_receiver_pins_are_left_alone(self):
        for pads in self.boards.values():
            for pin in IQ_PINS:
                self.assertIsNone(pads["U1", pin][0], pin)
            used = {pin for (ref, pin), (net, _, _) in pads.items() if ref == "U1" and net}
            self.assertEqual(used, {"5V", "GND", C5_TX, C5_RX})

    def test_the_bom_lists_every_part_of_a_board(self):
        with open(ROOT / "hardware/BOM.csv", newline="") as f:
            rows = list(csv.DictReader(f))
        for name, pads in self.boards.items():
            board = [r for r in rows if r["board"] == name.removeprefix("checkpoint-")]
            listed = [ref for r in board for ref in r["ref"].split()]
            self.assertEqual(sorted(listed), sorted({ref for ref, _ in pads}), name)
            for r in board:
                if r["ref"]:
                    self.assertEqual(int(r["qty"]), len(r["ref"].split()), r["part"])

    def test_header_rows_match_the_module_drawings(self):
        for pads in self.boards.values():
            for module in ("U1", "U2"):
                pins = [v for (ref, _), v in pads.items() if ref == module]
                self.assertEqual(len(pins), 18)
                xs = sorted({round(x, 3) for _, x, _ in pins})
                ys = sorted({round(y, 3) for _, _, y in pins})
                self.assertEqual(len(xs), 2)
                self.assertAlmostEqual(xs[1] - xs[0], 15.24, 3)
                self.assertEqual(len(ys), 9)
                self.assertAlmostEqual(ys[0], 1.59, 3, "the USB end flush with the top edge")
                self.assertAlmostEqual(ys[8] - ys[0], 8 * 2.54, 3)


if __name__ == "__main__":
    unittest.main()
