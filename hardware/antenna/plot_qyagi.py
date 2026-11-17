#!/usr/bin/env python3
"""Figures and figures of merit of the quasi-Yagi, from sim_qyagi.py's qyagi58-sim.json.

    python3 hardware/antenna/plot_qyagi.py     # needs numpy and matplotlib

Writes qyagi58-pattern.svg (E and H planes), qyagi58-s11.svg, qyagi58-lobe.png (the board
and its 3D gain lobe, seen at 45 degrees) and qyagi58-roll.svg (the beam as the gate sees it
for three roll angles), and prints the numbers README "Simulation" quotes.
"""
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_qyagi as g  # noqa: E402

SIM = HERE / "qyagi58-sim.json"
RACEBAND = np.array([5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]) / 1e3

# Reference palette of the docs' charts: categorical slots 1-3, the blue sequential ramp, ink.
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
RAMP = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b"]
INK, INK2, MUTED, GRID, SURFACE = "#0b0b0b", "#52514e", "#8a8984", "#e4e3df", "#fcfcfb"


def setup():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 9, "text.color": INK, "axes.labelcolor": INK2,
        "axes.edgecolor": MUTED, "xtick.color": INK2, "ytick.color": INK2,
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "svg.hashsalt": "qyagi58", "svg.fonttype": "path", "lines.linewidth": 2})
    return plt


def db_sum(*a):
    return 10 * np.log10(sum(10 ** (np.asarray(x) / 10) for x in a))


def load():
    d = json.loads(SIM.read_text())
    phi_e = np.array(d["cut"]["phi_e"])
    th = np.array(d["cut"]["theta_h"])
    # E plane (the board's): angle from the boresight (+Y) towards +X, the right dipole arm.
    a_e = (90 - phi_e + 180) % 360 - 180
    # H plane (normal to the board): from the boresight towards +Z, the copper side.
    a_h = np.concatenate([90 - th, 90 + th])
    a_h = (a_h + 180) % 360 - 180
    for pf in d["per_freq"]:
        oe, oh = np.argsort(a_e), np.argsort(a_h)
        pf["E"] = {"a": a_e[oe], "co": np.array(pf["e_phi"])[oe], "x": np.array(pf["e_theta"])[oe]}
        pf["H"] = {"a": a_h[oh], "co": np.concatenate(pf["h_phi"])[oh], "x": np.concatenate(pf["h_theta"])[oh]}
        for k in "EH":
            c = pf[k]
            _, u = np.unique(c["a"], return_index=True)     # +-90 come twice in the H plane
            for f in ("a", "co", "x"):
                c[f] = c[f][u]
            c["tot"] = db_sum(c["co"], c["x"])
    return d


def at(c, angle):
    return float(np.interp(angle, c["a"], c["tot"], period=360))


def hpbw(c):
    """-3 dB beamwidth around the boresight, interpolated."""
    a = np.arange(-180, 180.01, 0.1)
    v = np.interp(a, c["a"], c["tot"], period=360)
    peak = v[np.abs(a) <= 60].max()
    i0 = int(np.argmin(np.abs(a)))
    hi = next((a[i] for i in range(i0, len(a)) if v[i] < peak - 3), np.nan)
    lo = next((a[i] for i in range(i0, -1, -1) if v[i] < peak - 3), np.nan)
    return hi - lo                      # nan: the beam never falls 3 dB on one side


def summary(d):
    s = d["s11"]
    f, s11 = np.array(s["f_ghz"]), np.array(s["db"])
    band = (f >= RACEBAND[0] - 1e-6) & (f <= RACEBAND[-1] + 1e-6)
    fmin = f[np.argmin(s11)]
    under = f[s11 < -10]
    pi = np.array(s["pi_db"])
    under_pi = f[pi < -10]
    out = {"s11_band_worst": float(s11[band].max()), "s11_band_best": float(s11[band].min()),
           "s11_min_f": float(fmin), "s11_min": float(s11.min()),
           "s11_10db": [float(under.min()), float(under.max())] if len(under) else None,
           "pi_band_worst": float(pi[band].max()),
           "pi_10db": [float(under_pi.min()), float(under_pi.max())] if len(under_pi) else None, "freq": []}
    for pf in d["per_freq"]:
        E, H = pf["E"], pf["H"]
        bore = at(E, 0)
        out["freq"].append({
            "f": pf["f_ghz"], "gain_bore": bore, "gain_max": float(max(E["tot"].max(), H["tot"].max())),
            "realized_bore": bore + 10 * np.log10(pf["mismatch"]),
            "realized_pi": bore + 10 * np.log10(pf["pi_to_board"]), "eff": pf["eff"],
            "mismatch": pf["mismatch"], "dmax": pf["dmax_dbi"],
            "hpbw_e": hpbw(E), "hpbw_h": hpbw(H), "fb": bore - at(E, 180),
            "x_bore": float(np.interp(0, E["a"], E["x"])) - bore})
    return out


def polar_axis(fig, pos, title, rmin, rmax, left, right):
    ax = fig.add_subplot(pos, projection="polar")
    ax.set_theta_zero_location("N")
    ax.set_theta_direction(-1)
    ax.set_rlim(rmin, rmax)
    ticks = np.arange(np.ceil(rmin / 10) * 10, rmax + 0.1, 10)
    ax.set_rticks(ticks)
    ax.set_yticklabels([f"{t:+.0f}" if t else "0 dBi" for t in ticks], fontsize=7.5, color=INK2)
    ax.set_rlabel_position(157.5)
    ax.set_thetagrids(range(0, 360, 30), [f"{(t + 180) % 360 - 180:d}°" for t in range(0, 360, 30)])
    ax.grid(color=GRID, lw=0.8)
    ax.spines["polar"].set_color(GRID)
    ax.set_title(title, fontsize=10, pad=14, loc="center", color=INK)
    ax.text(0.5, -0.13, f"−90°: {left}   ·   +90°: {right}", transform=ax.transAxes,
            ha="center", va="top", fontsize=8, color=INK2)
    return ax


def fig_pattern(plt, d, sm):
    rmin, rmax = -30, 10
    fig = plt.figure(figsize=(9.2, 5.0))
    mid = sm["freq"][1]
    planes = (("E", f"E plane: the board's plane\nHPBW {mid['hpbw_e']:.0f}°", "left arm side", "right arm side"),
              ("H", f"H plane: square to the board\nHPBW {mid['hpbw_h']:.0f}°", "ground side", "copper side"))
    for k, (p, title, left, right) in enumerate(planes):
        ax = polar_axis(fig, 121 + k, title, rmin, rmax, left, right)
        for pf, col, lab in zip(d["per_freq"], (ORANGE, BLUE, AQUA), ("R1 5658 MHz", "5788 MHz", "R8 5917 MHz")):
            c = pf[p]
            t = np.deg2rad(np.append(c["a"], c["a"][0]))
            lw = 2 if col == BLUE else 1.2
            ax.plot(t, np.clip(np.append(c["tot"], c["tot"][0]), rmin, None), color=col, lw=lw,
                    label=f"gain, {lab}", zorder=3 if col == BLUE else 2)
        c = d["per_freq"][1][p]
        ax.plot(np.deg2rad(np.append(c["a"], c["a"][0])), np.clip(np.append(c["x"], c["x"][0]), rmin, None),
                color=MUTED, lw=1.2, ls=(0, (4, 2)), label="cross-polar, 5788 MHz")
    h, lab = ax.get_legend_handles_labels()
    fig.legend(h, lab, loc="lower center", ncol=4, frameon=False, fontsize=8, handlelength=2.2)
    fig.text(0.5, 0.955, "Quasi-Yagi qyagi58, simulated gain (openEMS, IEEE gain in dBi; 0° = boresight)",
             ha="center", fontsize=10.5, color=INK)
    fig.subplots_adjust(left=0.05, right=0.95, top=0.80, bottom=0.17, wspace=0.45)
    fig.savefig(HERE / "qyagi58-pattern.svg", metadata={"Date": None})
    plt.close(fig)


def fig_s11(plt, d):
    s = d["s11"]
    f, s11 = np.array(s["f_ghz"]), np.array(s["db"])
    fig, ax = plt.subplots(figsize=(6.4, 3.7))
    ax.axvspan(RACEBAND[0], RACEBAND[-1], color=GRID, lw=0)
    ax.text((RACEBAND[0] + RACEBAND[-1]) / 2, 1.0, "Raceband\nR1-R8", ha="center", va="top", fontsize=8, color=INK2)
    ax.axhline(-10, color=MUTED, lw=1, ls=(0, (4, 2)))
    ax.text(f[0] + 0.03, -10.6, "-10 dB", va="top", fontsize=8, color=INK2)
    pi = np.array(s["pi_db"])
    p = d["pi"]
    ax.plot(f, s11, color=MUTED, lw=1.4, ls=(0, (4, 2)), label="0 Ω in the series site, shunts empty")
    ax.plot(f, pi, color=BLUE, label=f"PI: series {p['series']}, shunts {p['shunt_sma']} (SMA side) "
                                      f"and {p['shunt_balun']} (balun side)")
    ax.set_xlim(f[0], f[-1])
    ax.set_ylim(min(-25, np.floor(min(s11.min(), pi.min()) / 5) * 5), 2)
    ax.set_xlabel("frequency (GHz)")
    ax.set_ylabel("S11 (dB)")
    ax.grid(axis="y", color=GRID, lw=0.8)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    ax.set_title("Simulated S11 at the SMA's pad, 50 Ω", fontsize=10, loc="left")
    fig.legend(*ax.get_legend_handles_labels(), loc="lower center", ncol=1, frameon=False, fontsize=7.5)
    fig.tight_layout(rect=(0, 0.13, 1, 1))
    fig.savefig(HERE / "qyagi58-s11.svg", metadata={"Date": None})
    plt.close(fig)


def grid3(d):
    """The band centre's total gain on the 3D grid, its phi seam closed: theta, phi (rad), dBi."""
    pf = d["per_freq"][1]
    th = np.deg2rad(np.array(d["grid"]["theta"]))
    ph = np.deg2rad(np.array(d["grid"]["phi"]))
    gt = db_sum(np.array(pf["g3_theta"]), np.array(pf["g3_phi"]))
    return th, np.append(ph, 2 * np.pi), np.concatenate([gt, gt[:, :1]], axis=1)


def gain_at(d, dx, dy, dz):
    """Total gain at the band's centre towards antenna-frame unit vectors (x dipole, y boresight, z normal)."""
    th, ph, gt = grid3(d)
    t = np.arccos(np.clip(dz, -1, 1))
    p = np.mod(np.arctan2(dy, dx), 2 * np.pi)
    i = np.clip(t / (th[1] - th[0]), 0, len(th) - 1.0001)          # bilinear on the 3D grid
    j = np.clip(p / (ph[1] - ph[0]), 0, len(ph) - 1.0001)
    i0, j0 = i.astype(int), j.astype(int)
    fi, fj = i - i0, j - j0
    return ((1 - fi) * (1 - fj) * gt[i0, j0] + fi * (1 - fj) * gt[i0 + 1, j0]
            + (1 - fi) * fj * gt[i0, j0 + 1] + fi * fj * gt[i0 + 1, j0 + 1])


def fig_lobe(plt, d):
    """The board standing up, its boresight up (the arm's up position), inside its gain lobe,
    seen 45 degrees between the copper face and the board's plane, from a little above."""
    from matplotlib.colors import LinearSegmentedColormap, Normalize
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    def view(p):                       # board frame (x dipole, y boresight, z copper side) -> plot
        p = np.asarray(p, dtype=float)
        return np.stack([p[..., 0], -p[..., 2], p[..., 1]], axis=-1)

    th, ph, gt = grid3(d)
    gmax = gt.max()
    floor = gmax - 25                            # the colour scale's bottom
    rmax = 80.0                                  # mm at the peak: the board is 47 mm long
    T, P = np.meshgrid(th, ph, indexing="ij")
    r = 10 ** ((gt - gmax) / 10) * rmax          # linear: the radius is the gain as a power ratio
    c0 = np.array([g.CX, g.YDRV, g.PCB_T / 2])  # the lobe starts at the driven dipole
    pts = view(c0 + r[..., None] * np.stack([np.sin(T) * np.cos(P), np.sin(T) * np.sin(P), np.cos(T)], -1))
    cmap = LinearSegmentedColormap.from_list("blue", RAMP)
    norm = Normalize(floor, gmax)
    light = np.array([0.5, -0.7, 0.5]) / np.linalg.norm([0.5, -0.7, 0.5])

    el, az = np.deg2rad(22), np.deg2rad(-45)
    cam = np.array([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)])
    centre = view([g.BW / 2, g.BH / 2, g.PCB_T / 2]) @ cam
    lobe = [], [], [], []                        # behind the board: polys, colours; in front: same
    for i in range(len(th) - 1):
        for j in range(len(ph) - 1):
            q = pts[[i, i + 1, i + 1, i], [j, j, j + 1, j + 1]]
            n = np.cross(q[2] - q[0], q[3] - q[1])
            k = 0.6 + 0.4 * abs(n @ light) / (np.linalg.norm(n) + 1e-12)
            col = np.array(cmap(norm(gt[i:i + 2, j:j + 2].mean())))
            col[:3] *= k
            col[3] = 0.35
            front = q.mean(0) @ cam > centre
            lobe[2 * front].append(q)
            lobe[2 * front + 1].append(col)
    layers = [(lobe[0], lobe[1])]                # drawn in this order, each sorted by depth
    t = g.PCB_T
    MASK, MASK_SIDE, COPPER, GND, GOLD = "#1f6b45", "#174f33", "#c9a227", "#9c7a1c", "#d4b04a"

    def quad(x0, y0, x1, y1, z):
        return view([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)])

    def block(x0, x1, y0, y1, z0, z1):
        return [view(f) for f in ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
                                  [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
                                  [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
                                  [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],
                                  [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)],
                                  [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)])]

    # the board seen from its copper side: ground, edges, mask, copper, then the SMA
    for pp, col in (([quad(*b, -0.06) for b in g.bot], GND), (block(0, g.BW, 0, g.BH, 0, t), MASK_SIDE),
                    ([quad(0, 0, g.BW, g.BH, t + 0.02)], MASK), ([quad(*b, t + 0.06) for b in g.top], COPPER),
                    (block(g.CX - 3.2, g.CX + 3.2, -9.0, 0, t / 2 - 3.2, t / 2 + 3.2), GOLD)):
        layers.append((pp, [col] * len(pp)))
    layers.append((lobe[2], lobe[3]))

    fig = plt.figure(figsize=(7.2, 7.0))
    ax = fig.add_subplot(projection="3d", computed_zorder=False)
    for pp, cc in layers:
        ax.add_collection3d(Poly3DCollection(pp, facecolors=cc, edgecolors="none", linewidths=0,
                                             zsort="average"))
    ie = int(np.argmin(np.abs(th - np.pi / 2)))           # theta 90: the E plane, the board's
    e = pts[ie, :]
    ax.plot(e[:, 0], e[:, 1], e[:, 2], color=ORANGE, lw=1.5, alpha=0.9)
    jh = [int(np.argmin(np.abs(ph - v))) for v in (np.pi / 2, 3 * np.pi / 2)]
    h = np.concatenate([pts[:, jh[0]], pts[::-1, jh[1]]])  # phi 90/270: the H plane
    ax.plot(h[:, 0], h[:, 1], h[:, 2], color=AQUA, lw=1.5, alpha=0.9)
    top = view(c0)
    ax.quiver(top[0], top[1], g.BH + 2, 0, 0, 1, length=rmax + c0[1] - g.BH + 8, color=INK, lw=1.3,
              arrow_length_ratio=0.1)
    lo, hi = pts.reshape(-1, 3).min(0), pts.reshape(-1, 3).max(0)
    lo[2] = min(lo[2], -10)
    hi[2] = max(hi[2], c0[1] + rmax + 8)
    ax.set_xlim(lo[0], hi[0])
    ax.set_ylim(lo[1], hi[1])
    ax.set_zlim(lo[2], hi[2])
    ax.set_box_aspect(hi - lo)
    ax.view_init(elev=np.rad2deg(el), azim=np.rad2deg(az))
    ax.set_axis_off()
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    cb = fig.colorbar(sm, ax=ax, shrink=0.45, pad=0.0, aspect=16)
    cb.set_label("gain (dBi), 5788 MHz", color=INK2)
    cb.outline.set_visible(False)
    s = summary(d)["freq"][1]
    ax.legend(handles=[plt.Line2D([], [], color=ORANGE, lw=1.5,
                                  label=f"E plane: the board's plane ({s['hpbw_e']:.0f}° at -3 dB)"),
                       plt.Line2D([], [], color=AQUA, lw=1.5,
                                  label=f"H plane: square to the board ({s['hpbw_h']:.0f}°)"),
                       plt.Line2D([], [], color=INK, lw=1.3, label="boresight")],
              loc="lower left", frameon=False, fontsize=8)
    ax.set_title("qyagi58 standing up in its simulated gain lobe, seen at 45°\n"
                 "radius: the gain as a power ratio (the polar plots are in dB)", fontsize=10, color=INK)
    fig.subplots_adjust(left=0, right=0.94, top=0.94, bottom=0.02)
    fig.savefig(HERE / "qyagi58-lobe.png", dpi=150, metadata={"Software": None})
    plt.close(fig)


def fig_roll(plt, d):
    """The beam as the gate sees it, standing behind the antenna: -3/-6/-10 dB regions over
    azimuth and elevation, for the dipole horizontal (roll 0), at 45 and vertical (roll 90)."""
    from matplotlib.patches import Patch
    a = np.deg2rad(np.linspace(-90, 90, 181))
    A, Ev = np.meshgrid(a, a)
    F = np.cos(Ev) * np.cos(A)          # forward
    R = np.cos(Ev) * np.sin(A)          # right
    U = np.sin(Ev)                      # up
    peak = grid3(d)[2].max()
    fig, axs = plt.subplots(1, 3, figsize=(9.2, 4.1), sharey=True)
    levels = [-99, -10, -6, -3, 0.1]
    colors = ["#f0efec", RAMP[1], RAMP[3], RAMP[5]]
    for ax, roll in zip(axs, (0, 45, 90)):
        rr = np.deg2rad(roll)
        dx = np.cos(rr) * R + np.sin(rr) * U       # the dipole, rolled from horizontal
        dz = -np.sin(rr) * R + np.cos(rr) * U      # the board's normal
        G = gain_at(d, dx, F, dz) - peak
        ax.contourf(np.rad2deg(A), np.rad2deg(Ev), G, levels=levels, colors=colors)
        ax.contour(np.rad2deg(A), np.rad2deg(Ev), G, levels=[-10, -6, -3], colors=[SURFACE], linewidths=0.6)
        L = 80
        ax.plot([-L * np.cos(rr), L * np.cos(rr)], [-L * np.sin(rr), L * np.sin(rr)], color=ORANGE, lw=1.6)
        ax.set_title(f"roll {roll}°", fontsize=10)
        ax.set_aspect("equal")
        ax.set_xlim(-90, 90)
        ax.set_ylim(-90, 90)
        ax.set_xticks([-90, -45, 0, 45, 90])
        ax.set_yticks([-90, -45, 0, 45, 90])
        ax.set_xlabel("azimuth (°)")
        for sp in ax.spines.values():
            sp.set_color(GRID)
    axs[0].set_ylabel("elevation (°)")
    fig.suptitle("The beam seen from behind the antenna, 5788 MHz", fontsize=10.5, color=INK)
    hs = [Patch(color=colors[3], label="within 3 dB of the peak"), Patch(color=colors[2], label="3 to 6 dB down"),
          Patch(color=colors[1], label="6 to 10 dB down"), Patch(color=colors[0], label="more than 10 dB down"),
          plt.Line2D([], [], color=ORANGE, lw=1.6, label="the dipole's direction")]
    fig.legend(handles=hs, loc="lower center", ncol=5, frameon=False, fontsize=8)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(HERE / "qyagi58-roll.svg", metadata={"Date": None})
    plt.close(fig)


def main():
    d = load()
    sm = summary(d)
    plt = setup()
    fig_pattern(plt, d, sm)
    fig_s11(plt, d)
    fig_lobe(plt, d)
    fig_roll(plt, d)
    print(f"S11 with a 0 ohm: {sm['s11_min']:.1f} dB at {sm['s11_min_f']:.3f} GHz; Raceband between "
          f"{sm['s11_band_best']:.1f} and {sm['s11_band_worst']:.1f} dB; under -10 dB: {sm['s11_10db']}")
    print(f"S11 with the PI {d['pi']['series']} / {d['pi']['shunt_sma']} / {d['pi']['shunt_balun']}: worst in "
          f"the Raceband {sm['pi_band_worst']:.1f} dB, under -10 dB: {sm['pi_10db']}; by part count: "
          f"{d['pi']['best_by_count']}")
    for f in sm["freq"]:
        print(f"{f['f'] * 1e3:.0f} MHz: gain {f['gain_bore']:.1f} dBi at the boresight (max {f['gain_max']:.1f}, "
              f"Dmax {f['dmax']:.1f}), realized {f['realized_bore']:.1f} dBi (with the PI "
              f"{f['realized_pi']:.1f}), efficiency {f['eff'] * 100:.0f} %, "
              f"mismatch {f['mismatch'] * 100:.0f} %, HPBW E {f['hpbw_e']:.0f} deg / H {f['hpbw_h']:.0f} deg, "
              f"F/B {f['fb']:.1f} dB, cross-pol {f['x_bore']:.1f} dB")


if __name__ == "__main__":
    main()
