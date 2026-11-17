#!/usr/bin/env python3
"""openEMS FDTD model of the quasi-Yagi: S11, gain, radiation pattern and the PI's values.

    python3 hardware/antenna/sim_qyagi.py            # simulate (5 runs, ~10 min), save, plot
    python3 hardware/antenna/sim_qyagi.py --post     # reprocess the runs in --work, plot
    python3 hardware/antenna/sim_qyagi.py --plot     # plot again from qyagi58-sim.json

The model is built from gen_qyagi.py's own geometry, so it follows its parameters. Simulating
needs openEMS and its Python bindings (CSXCAD, openEMS) with numpy and h5py; plotting needs
numpy and matplotlib only. The field data stays in --work (default: a temporary folder);
qyagi58-sim.json keeps what the figures need. What the model leaves out: README "Simulation".

Two models share one mesh:
- the antenna with a 0 ohm in the series footprint, fed at the SMA's pad: S11, gain, pattern;
- four ports, at the SMA's pad and on the PI's three 0402 sites (series, both shunts): their
  4x4 S-matrix, on which every pair of parts is tried by circuit algebra. The PI's values
  come from there.

Frame: gen_qyagi's, in mm. The board lies in the XY plane, top copper at z = PCB_T, the
ground on the bottom face at z = 0; it radiates towards +Y and the dipole runs along X.
"""
import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_qyagi as g  # noqa: E402

OUT = HERE / "qyagi58-sim.json"
RACEBAND = [5658e6, 5695e6, 5732e6, 5769e6, 5806e6, 5843e6, 5880e6, 5917e6]   # R1..R8
FREQS = [RACEBAND[0], 5788e6, RACEBAND[-1]]   # R1, the band's centre, R8
F0, FC = 5.8e9, 1.5e9                         # Gaussian excitation: 4.3 to 7.3 GHz
AIR = 35.0                                    # air around the board (the PML takes 8 cells of it)
FINE, COARSE = 0.5, 2.0                       # mesh on the board, in the air (< lambda/20 at 7.3 GHz)
MERGE = 0.12                                  # copper edges closer than this share a mesh line
STEP_3D, STEP_CUT = 3, 1                      # far-field grid, degrees
Z0 = 376.730313668

# 0402 parts tried on the PI: RF inductors in 0.1 nH steps up to 2 nH then E12/E24, RF capacitors
# likewise in pF. Parasitics: a capacitor's (and the 0 ohm's) mounted ESL, an inductor's Q.
L_NH = [round(0.3 + 0.1 * i, 1) for i in range(18)] + [2.2, 2.4, 2.7, 3.0, 3.3, 3.6, 3.9, 4.3, 4.7,
                                                        5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 10.0]
C_PF = [round(0.2 + 0.1 * i, 1) for i in range(19)] + [2.2, 2.4, 2.7, 3.0, 3.3, 3.9, 4.7, 5.6, 6.8,
                                                        8.2, 10.0]
ESL, ESR, Q_L = 0.35e-9, 0.2, 30.0


def mesh_lines(fixed, lo, hi, fine, coarse, ratio=1.4):
    """Mesh lines on every copper edge, cells <= fine between them, growing to coarse outside."""
    keep = []
    for p in sorted(fixed):
        if not keep or p - keep[-1] >= MERGE:
            keep.append(p)
    out = [keep[0]]
    for a, b in zip(keep, keep[1:]):
        n = int(np.ceil((b - a) / fine - 1e-9))
        out += list(np.linspace(a, b, n + 1)[1:])

    def grade(x, end, step):
        res, d = [], np.sign(end - x)
        while (rem := (end - x) * d) > 1e-9:
            step = min(step * ratio, coarse)
            if step >= coarse or rem <= step:      # the rest in equal cells, none over coarse
                n = int(np.ceil(rem / coarse - 1e-9))
                return res + list(x + d * np.linspace(0, rem, n + 1)[1:])
            x += d * step
            res.append(x)
        return res

    return np.array(grade(out[0], lo, out[1] - out[0])[::-1] + out + grade(out[-1], hi, out[-1] - out[-2]))


def pi_sites():
    """The PI's three 0402 sites as lumped-port boxes: series across the line's cut, each shunt
    across the gap between its line pad and its ground pad."""
    t = g.PCB_T
    (l1, g1), (l2, g2) = g.SH1, g.SH2
    return [("series", [g.CX - 0.25, g.YS - 0.25, t], [g.CX + 0.25, g.YS + 0.25, t], "y"),
            ("shunt, SMA side", [g1[2], l1[1], t], [l1[0], l1[3], t], "x"),
            ("shunt, balun side", [l2[2], l2[1], t], [g2[0], l2[3], t], "x")]


def build(excite=None):
    """excite None: the antenna, a 0 ohm in the series footprint, fed at the SMA's pad.
    excite k (0..3): the four-port model, port k fed, the others 50 ohm."""
    if not hasattr(np, "float"):   # openEMS 0.0.35's ports.py still uses np.float
        np.float = float
    from CSXCAD import ContinuousStructure
    from openEMS import openEMS
    from openEMS.physical_constants import EPS0

    fdtd = openEMS(EndCriteria=1e-4, NrTS=200000)
    fdtd.SetGaussExcite(F0, FC)
    fdtd.SetBoundaryCond(["PML_8"] * 6)
    csx = ContinuousStructure()
    fdtd.SetCSX(csx)
    grid = csx.GetGrid()
    grid.SetDeltaUnit(1e-3)
    t = g.PCB_T

    fr4 = csx.AddMaterial("FR4", epsilon=g.ER, kappa=2 * np.pi * F0 * EPS0 * g.ER * g.TAN_D)
    fr4.AddBox([0, 0, 0], [g.BW, g.BH, t], priority=0)
    zero_ohm = g.r(g.CX - 0.25, g.YS - 0.25, g.CX + 0.25, g.YS + 0.25)   # 0402 body width
    top = csx.AddMetal("top")
    for x1, y1, x2, y2 in g.top + ([zero_ohm] if excite is None else []):
        top.AddBox([x1, y1, t], [x2, y2, t], priority=10)
    bot = csx.AddMetal("bottom")
    for x1, y1, x2, y2 in g.bot:
        bot.AddBox([x1, y1, 0], [x2, y2, 0], priority=10)
    feed = ([g.xl_in, 0, 0], [g.xr_in, 0, t], "z")
    boxes = [feed] + ([] if excite is None else [s[1:] for s in pi_sites()])
    ports = [fdtd.AddLumpedPort(k + 1, 50, a, b, d, excite=1.0 if k == (excite or 0) else 0, priority=5)
             for k, (a, b, d) in enumerate(boxes)]

    rects = g.top + g.bot + [zero_ohm]
    xs = [0, g.BW, g.xl_in, g.xr_in] + [v for p in rects for v in (p[0], p[2])]
    ys = [0, g.BH] + [v for p in rects for v in (p[1], p[3])]
    lx = mesh_lines(xs, -AIR, g.BW + AIR, FINE, COARSE)
    ly = mesh_lines(ys, -AIR, g.BH + AIR, FINE, COARSE)
    grid.SetLines("x", lx)
    grid.SetLines("y", ly)
    grid.SetLines("z", mesh_lines(list(np.linspace(0, t, 5)), -AIR, t + AIR, t / 4, COARSE))
    # A via is a wire on the nearest mesh line (within a quarter cell, inside its pad).
    vias = csx.AddMetal("vias")
    for x, y in g.vias:
        sx, sy = lx[np.argmin(np.abs(lx - x))], ly[np.argmin(np.abs(ly - y))]
        vias.AddBox([sx, sy, 0], [sx, sy, t], priority=10)
    nf2ff = fdtd.CreateNF2FFBox(frequency=FREQS) if excite is None else None
    return fdtd, ports, nf2ff, [len(grid.GetLines(d)) for d in "xyz"]


def angles_cut():
    """E plane: the board's plane (theta 90). H plane: the YZ plane (phi 90 and 270)."""
    return np.arange(0, 360, STEP_CUT, dtype=float), np.arange(0, 181, STEP_CUT, dtype=float)


def smatrix(work, f):
    """The four-port S-matrix, [freq, j, k], from the four runs in work/port1..4."""
    S = np.zeros((len(f), 4, 4), complex)
    for k in range(4):
        _, ports, _, _ = build(excite=k)
        for p in ports:
            p.CalcPort(str(work / f"port{k + 1}"), f)
        for j, p in enumerate(ports):
            S[:, j, k] = p.uf_ref / ports[k].uf_inc
    return S


def parts(f):
    """Every candidate for a site: (label, impedance over f). None: the site left empty."""
    w = 2 * np.pi * f
    out = [("0 Ω", 1j * w * ESL)]
    out += [(f"{v:g} nH", 1j * w * v * 1e-9 + w * v * 1e-9 / Q_L) for v in L_NH]
    out += [(f"{v:g} pF", 1 / (1j * w * v * 1e-12) + 1j * w * ESL + ESR) for v in C_PF]
    return out


def pi_search(S, f):
    """Gamma at the SMA for every series part and pair of shunts (or none), and the power that
    reaches the board past the parts; returns the best of each part count by worst R1-R8 S11."""
    gam = lambda z: (z - 50) / (z + 50)
    cand = parts(f)
    series = [(lab, gam(z)) for lab, z in cand]
    shunt = [("—", np.ones(len(f), complex))] + [(lab, gam(z)) for lab, z in cand[1:]]
    S11, S1L, SL1, SLL = S[:, 0, 0], S[:, 0, 1:], S[:, 1:, 0], S[:, 1:, 1:]
    best = {}
    gs = np.array([x[1] for x in series])            # [ns, f]
    gp = np.array([x[1] for x in shunt])             # [np, f]
    n1, n2 = np.meshgrid(np.arange(len(shunt)), np.arange(len(shunt)), indexing="ij")
    n1, n2 = n1.ravel(), n2.ravel()
    for i, (lab_s, _) in enumerate(series):
        GL = np.zeros((len(n1), len(f), 3, 3), complex)
        GL[:, :, 0, 0] = gs[i]
        GL[:, :, 1, 1] = gp[n1]
        GL[:, :, 2, 2] = gp[n2]
        M = np.eye(3) - SLL[None] @ GL
        x = np.linalg.solve(M, SL1[None, :, :, None])[..., 0]          # b_L = (I - S_LL G_L)^-1 S_L1
        gin = S11[None] + np.einsum("fj,nfjk,nfk->nf", S1L, GL, x)
        worst = np.abs(gin).max(axis=1)
        for m in range(len(n1)):
            count = 1 + (n1[m] > 0) + (n2[m] > 0) - (lab_s == "0 Ω")
            key = int(count)
            if key not in best or worst[m] < best[key][0]:
                best[key] = (worst[m], lab_s, shunt[n1[m]][0], shunt[n2[m]][0])
    return best


def pi_response(S, f, choice):
    """Gamma at the SMA and the fraction of the incident power that reaches the board."""
    cand = dict(parts(f))
    gam = lambda z: (z - 50) / (z + 50)
    gl = [gam(cand[choice[0]])] + [np.ones(len(f)) if c == "—" else gam(cand[c]) for c in choice[1:]]
    out_g, out_p = [], []
    for n in range(len(f)):
        GL = np.diag([x[n] for x in gl])
        A = np.eye(3) - S[n, 1:, 1:] @ GL
        b_l = np.linalg.solve(A, S[n, 1:, 0])        # waves leaving the board into the parts
        a_l = GL @ b_l                               # and coming back
        b1 = S[n, 0, 0] + S[n, 0, 1:] @ a_l
        lost = np.sum(np.abs(b_l) ** 2 - np.abs(a_l) ** 2)   # dissipated in the parts
        out_g.append(b1)
        out_p.append(1 - abs(b1) ** 2 - lost)
    return np.array(out_g), np.array(out_p)


def post(work):
    f = np.linspace(4.5e9, 7.2e9, 271)
    _, (port,), nf2ff, cells = build()
    port.CalcPort(str(work / "antenna"), f)
    s11 = port.uf_ref / port.uf_inc
    zin = port.uf_tot / port.if_tot
    port.CalcPort(str(work / "antenna"), np.array(FREQS))
    p_acc, p_inc = np.real(port.P_acc), np.real(port.P_inc)

    def gains(res, i):
        """IEEE gain (dBi) of each polarisation: 4 pi U / accepted power."""
        k = 4 * np.pi * float(res.r) ** 2 / (2 * Z0) / p_acc[i]
        return [10 * np.log10(np.maximum(k * np.abs(e[i]) ** 2, 1e-12)) for e in (res.E_theta, res.E_phi)]

    aw = str(work / "antenna")
    th3 = np.arange(0, 181, STEP_3D, dtype=float)
    ph3 = np.arange(0, 360, STEP_3D, dtype=float)
    r3 = nf2ff.CalcNF2FF(aw, FREQS, th3, ph3, outfile="ff3d.h5")
    phi_e, th_h = angles_cut()
    re = nf2ff.CalcNF2FF(aw, FREQS, [90.0], phi_e, outfile="ff_e.h5")
    rh = nf2ff.CalcNF2FF(aw, FREQS, th_h, [90.0, 270.0], outfile="ff_h.h5")

    # the PI: four-port S-matrix over the plot's grid and the Raceband, then the search
    fb = np.array(RACEBAND)
    Sb = smatrix(work, fb)
    best = pi_search(Sb, fb)
    one = min(best[k][0] for k in best)
    # the fewest parts within 1 dB of the best worst-case S11
    pick = min(k for k in best if 20 * np.log10(best[k][0]) <= 20 * np.log10(one) + 1.0)
    choice = best[pick][1:]
    Sf = smatrix(work, f)
    g_pi, _ = pi_response(Sf, f, choice)
    _, p_board = pi_response(smatrix(work, np.array(FREQS)), np.array(FREQS), choice)

    rnd = lambda a: np.round(np.asarray(a, dtype=float), 2).tolist()
    data = {"model": {"er": g.ER, "tan_d": g.TAN_D, "pcb_t": g.PCB_T, "cells": cells,
                      "mesh_mm": [FINE, COARSE], "air_mm": AIR},
            "s11": {"f_ghz": rnd(f / 1e9), "db": rnd(20 * np.log10(np.abs(s11))),
                    "z_re": rnd(zin.real), "z_im": rnd(zin.imag), "pi_db": rnd(20 * np.log10(np.abs(g_pi)))},
            "pi": {"series": choice[0], "shunt_sma": choice[1], "shunt_balun": choice[2],
                   "best_by_count": {str(k): [round(20 * np.log10(v[0]), 2), *v[1:]] for k, v in sorted(best.items())},
                   "esl_nh": ESL * 1e9, "esr_ohm": ESR, "q_l": Q_L},
            "freqs_ghz": [x / 1e9 for x in FREQS], "per_freq": [],
            "grid": {"theta": th3.tolist(), "phi": ph3.tolist()},
            "cut": {"phi_e": phi_e.tolist(), "theta_h": th_h.tolist()}}
    for i, fr in enumerate(FREQS):
        g3 = gains(r3, i)
        ge = [x[0, :] for x in gains(re, i)]          # theta 90, phi 0..359
        gh = gains(rh, i)                             # theta 0..180, phi 90 / 270
        data["per_freq"].append({
            "f_ghz": fr / 1e9, "eff": round(float(r3.Prad[i] / p_acc[i]), 3),
            "mismatch": round(float(p_acc[i] / p_inc[i]), 3), "pi_to_board": round(float(p_board[i]), 3),
            "dmax_dbi": round(float(10 * np.log10(r3.Dmax[i])), 2),
            "e_theta": rnd(ge[0]), "e_phi": rnd(ge[1]),
            "h_theta": [rnd(gh[0][:, 0]), rnd(gh[0][:, 1])], "h_phi": [rnd(gh[1][:, 0]), rnd(gh[1][:, 1])],
            # the 3D grid only at the band's centre: it is what the lobe figure needs
            **({"g3_theta": np.round(g3[0], 1).tolist(), "g3_phi": np.round(g3[1], 1).tolist()}
               if i == 1 else {})})
    OUT.write_text(json.dumps(data, separators=(",", ":")) + "\n")
    print("PI:", " / ".join(choice), f"worst S11 in R1-R8 {20 * np.log10(best[pick][0]):.1f} dB;",
          "best by part count:", data["pi"]["best_by_count"])
    print("wrote", OUT.name, f"{OUT.stat().st_size / 1e3:.0f} kB")


def simulate(work):
    work.mkdir(parents=True, exist_ok=True)
    for name, excite in [("antenna", None)] + [(f"port{k + 1}", k) for k in range(4)]:
        fdtd, _, _, cells = build(excite)
        print(name, "mesh", " x ".join(map(str, cells)), f"= {np.prod(cells) / 1e6:.2f} M cells", flush=True)
        fdtd.Run(str(work / name), cleanup=True, verbose=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", type=Path, help="openEMS run folder (kept)")
    ap.add_argument("--post", action="store_true", help="reprocess the runs in --work, then plot")
    ap.add_argument("--plot", action="store_true", help="only plot from " + OUT.name)
    a = ap.parse_args()
    if not a.plot:
        work = a.work or Path(tempfile.mkdtemp(prefix="qyagi58-openems-"))
        print("runs in", work)
        if not a.post:
            simulate(work)
        post(work)
    import plot_qyagi
    plot_qyagi.main()


if __name__ == "__main__":
    main()
