#!/usr/bin/env python3
#!/usr/bin/env python3
"""Printed 5.8 GHz quasi-Yagi antenna on 1.6 mm FR4 (er ~ 4.4): Gerber RS-274X + Excellon.

    python3 hardware/antenna/gen_qyagi.py    # needs matplotlib and gerbonara

This script is the source of the antenna: change it, not the Gerbers. It writes
hardware/gerbers/qyagi58-<version>-gerbers.zip, reads it back with gerbonara and renders the preview.
Why it is the way it is and what is still open: hardware/antenna/README.md.

Units: mm. Origin: bottom left corner of the board. Radiates towards +Y.
"""
import shutil
import subprocess
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
VERSION = "v2"               # bump it after an order: printed on the board, names the zip
ZIP = HERE.parent / "gerbers" / f"qyagi58-{VERSION}-gerbers.zip"
files = {}                   # name in the zip -> Gerber or drill text

BW, BH = 34.0, 47.0          # board
CX = BW / 2                  # axis
YG = 16.0                    # edge of the ground plane (the reflector)
W50 = 3.0                    # 50 ohm microstrip
W100 = 0.7                   # the balun's 100 ohm branches
YT = 8.0                     # T junction
YDRV = YG + 9.5              # dipole
WD = 1.5                     # width of the dipole and directors
L_ARM = 9.5                  # length of one arm
GAP = 3.0                    # the dipole's centre gap (= CPS spacing)
L_DIR = 16.5
YDIR = [YDRV + 9.0, YDRV + 18.0]
XLOOP = 26.1                 # reach of the balun's delay (~lambda_g/2)
YLOOP = 12.0
EDGE = 0.3                   # ground plane to the left and right board edges
SILK = 1.2                   # text size of the spec block

def r(x1, y1, x2, y2):
    return (min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2))

xl_in, xr_in = CX - GAP / 2, CX + GAP / 2          # 15.5 / 18.5
h = W100 / 2
top = []
# --- 50 ohm microstrip, cut for the PI's series 0402 ---
YS = 5.0   # centre of the series part
top += [r(xl_in, 0, xr_in, YS - 0.25), r(xl_in, YS + 0.25, xr_in, YT + h)]
# --- the PI's shunts (DNP): a pad on the line, a ground pad with a via ---
top += [r(xl_in - 0.6, 2.7, xl_in, 3.3), r(13.0, 2.6, 14.4, 3.4)]          # shunt, SMA side
top += [r(xr_in, 6.0, xr_in + 0.6, 6.6), r(19.6, 5.9, 21.0, 6.7)]          # shunt, balun side
# --- ground pads of the edge-launch SMA ---
top += [r(10.0, 0, 12.0, 5.0), r(22.0, 0, 24.0, 5.0)]
# --- balun: direct left branch ---
top += [r(xl_in - W100, YT - h, xl_in, YDRV - WD / 2)]
# --- balun: right branch, with the delay ---
top += [r(xr_in, YT - h, XLOOP + h, YT + h),
        r(XLOOP - h, YT - h, XLOOP + h, YLOOP + h),
        r(xr_in, YLOOP - h, XLOOP + h, YLOOP + h),
        r(xr_in, YLOOP - h, xr_in + W100, YDRV - WD / 2)]
# --- driven dipole ---
top += [r(xl_in - L_ARM, YDRV - WD / 2, xl_in, YDRV + WD / 2),
        r(xr_in, YDRV - WD / 2, xr_in + L_ARM, YDRV + WD / 2)]
# --- directors ---
for y in YDIR:
    top.append(r(CX - L_DIR / 2, y - WD / 2, CX + L_DIR / 2, y + WD / 2))

bot = [r(EDGE, 0, BW - EDGE, YG)]                      # ground = reflector

fmask = [r(xl_in, 0, xr_in, 4.0),                          # SMA centre pin
         r(10.0, 0, 12.0, 5.0), r(22.0, 0, 24.0, 5.0),     # SMA grounds
         r(16.6, YS - 0.85, 17.4, YS - 0.25), r(16.6, YS + 0.25, 17.4, YS + 0.85),  # series 0402
         r(14.9, 2.7, 15.5, 3.3), r(13.8, 2.7, 14.4, 3.3),  # shunt 1
         r(18.5, 6.0, 19.1, 6.6), r(19.6, 6.0, 20.2, 6.6)]  # shunt 2
bmask = [r(10.0, 0, 12.0, 5.0), r(22.0, 0, 24.0, 5.0)]

vias = [(13.4, 3.0), (20.6, 6.3), (11.0, 1.5), (11.0, 3.5), (23.0, 1.5), (23.0, 3.5)]
VIA_DRILL = 0.3

def c(v):  # 4.6 format, in mm
    return f"{int(round(v * 1e6))}"

def gerber(name, func, rects, polarity_note=""):
    L = ["%TF.GenerationSoftware,Claude,qyagi58*%",
         f"%TF.FileFunction,{func}*%",
         "%FSLAX46Y46*%", "%MOMM*%", "%LPD*%", "G01*",
         "%ADD10C,0.100000*%", "D10*"]
    for x1, y1, x2, y2 in rects:
        L += ["G36*", f"X{c(x1)}Y{c(y1)}D02*", f"X{c(x2)}Y{c(y1)}D01*",
              f"X{c(x2)}Y{c(y2)}D01*", f"X{c(x1)}Y{c(y2)}D01*",
              f"X{c(x1)}Y{c(y1)}D01*", "G37*"]
    L.append("M02*")
    files[name] = "\n".join(L) + "\n"

gerber("qyagi58.GTL", "Copper,L1,Top", top)
gerber("qyagi58.GBL", "Copper,L2,Bot", bot)
gerber("qyagi58.GTS", "Soldermask,Top", fmask)
gerber("qyagi58.GBS", "Soldermask,Bot", bmask)

# --- silkscreen (text as outlines -> regions, the letters' counters in clear polarity) ---
from matplotlib.textpath import TextPath
from matplotlib.font_manager import FontProperties

FONT = FontProperties(family="DejaVu Sans", weight="bold")

def silk_text(txt, x, y, size, anchor="middle"):
    tp = TextPath((0, 0), txt, size=size, prop=FONT)
    bb = tp.get_extents()
    dx = x - (bb.x0 + bb.width / 2 if anchor == "middle" else bb.x0)
    polys = [[(px + dx, py + y) for px, py in p] for p in tp.to_polygons()]
    return polys

def area(p):
    return sum(p[i][0] * p[i - 1][1] - p[i - 1][0] * p[i][1] for i in range(len(p))) / 2

silk = []
silk += silk_text(f"FPV 5.8 Checkpoint {VERSION}", CX, 45.0, 1.6)
spec = ["Quasi-Yagi 5.8 GHz", "5658-5917 MHz", "Raceband R1-R8", "50 Ω · linear pol."]
for i, line in enumerate(spec):
    silk += silk_text(line, 0.8, 14.4 - i * 1.7, SILK, anchor="start")

def gerber_poly(name, func, polys):
    L = ["%TF.GenerationSoftware,Claude,qyagi58*%", f"%TF.FileFunction,{func}*%",
         "%FSLAX46Y46*%", "%MOMM*%", "G01*", "%ADD10C,0.100000*%", "D10*"]
    # outer contours dark, counters clear
    outer_sign = 1 if sum(area(p) for p in polys) > 0 else -1
    outers = [p for p in polys if area(p) * outer_sign > 0]
    holes = [p for p in polys if area(p) * outer_sign <= 0]
    for pol, group in (("%LPD*%", outers), ("%LPC*%", holes)):
        L.append(pol)
        for p in group:
            L += ["G36*", f"X{c(p[0][0])}Y{c(p[0][1])}D02*"]
            L += [f"X{c(px)}Y{c(py)}D01*" for px, py in p[1:]]
            L += [f"X{c(p[0][0])}Y{c(p[0][1])}D01*", "G37*"]
    L.append("M02*")
    files[name] = "\n".join(L) + "\n"

gerber_poly("qyagi58.GTO", "Legend,Top", silk)

# outline
L = ["%TF.GenerationSoftware,Claude,qyagi58*%", "%TF.FileFunction,Profile,NP*%",
     "%FSLAX46Y46*%", "%MOMM*%", "%LPD*%", "G01*", "%ADD10C,0.100000*%", "D10*",
     f"X0Y0D02*", f"X{c(BW)}Y0D01*", f"X{c(BW)}Y{c(BH)}D01*", f"X0Y{c(BH)}D01*", "X0Y0D01*", "M02*"]
files["qyagi58.GKO"] = "\n".join(L) + "\n"

# Excellon
L = ["M48", "METRIC,TZ", "FMAT,2", f"T1C{VIA_DRILL:.3f}", "%", "G90", "G05", "T1"]
for x, y in vias:
    L.append(f"X{x:.3f}Y{y:.3f}")
L.append("M30")
files["qyagi58.DRL"] = "\n".join(L) + "\n"

# A fixed date, so the same parameters give the same zip.
with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(files):
        info = zipfile.ZipInfo(name, date_time=(2026, 11, 7, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, files[name])

# Read back what the fab will get, before anyone looks at the preview.
from gerbonara import LayerStack

stack = LayerStack.open(ZIP)
want = {("top", "copper"), ("bottom", "copper"), ("top", "mask"), ("bottom", "mask"), ("top", "silk"),
        ("mechanical", "outline")}
missing = want - set(stack.graphic_layers)
if missing:
    raise SystemExit(f"gerbonara does not see {sorted(missing)}")
if sum(len(d.objects) for d in stack.drill_layers) != len(vias):
    raise SystemExit("gerbonara does not read the drill file back")
(x0, y0), (x1, y1) = stack.bounding_box()
if abs((x1 - x0) - BW) > 0.2 or abs((y1 - y0) - BH) > 0.2:
    raise SystemExit(f"read back a {x1 - x0:.2f} x {y1 - y0:.2f} mm board, not {BW} x {BH}")

svg = HERE / "qyagi58.svg"
svg.write_text(str(stack.to_pretty_svg(side="top")))
if shutil.which("rsvg-convert"):
    subprocess.run(["rsvg-convert", "-z", "16", "-b", "white", "-o", str(HERE / "qyagi58.png"), str(svg)], check=True)
else:
    print("no rsvg-convert: the PNG preview is not updated")
print("ok", ZIP.name, f"{BW:g} x {BH:g} mm", sorted(files))
