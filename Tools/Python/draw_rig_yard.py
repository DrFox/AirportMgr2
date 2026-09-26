"""Draws the M_RigTest reversing yard to scale (spec 2026-09-26 section 4).

Figures copied from FRigYardLayout (Source/AirportMgr/RigTestCourse.h); run after changing either.
Balloon reach is UTurnGeom's ~27 m past a dead end for the bowser (2026-09-25, measured).
Output: docs/superpowers/plans/2026-09-26-rig-yard.png. Needs matplotlib; no editor.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

N = {  # node positions, uu
    "NW": (-2000, -7000), "W": (-6500, -7000), "P1": (3000, -7000), "J": (7000, -7000), "P2": (10500, -7000),
    "NE": (13000, -7000), "Bay90": (7000, -11000), "S": (13000, -12000), "SE": (13000, -16000),
    "SW": (-2000, -16000), "H": (16000, -12000), "D": (19500, -12000), "Hammer": (16000, -8000),
}
ROADS = [("W", "NW"), ("NW", "P1"), ("P1", "J"), ("J", "P2"), ("P2", "NE"), ("J", "Bay90"), ("NE", "S"), ("S", "SE"),
         ("SE", "SW"), ("SW", "NW"), ("S", "H"), ("H", "D"), ("H", "Hammer")]
DEAD_ENDS = {"W": (-1, 0), "Bay90": (0, -1), "D": (1, 0), "Hammer": (0, 1)}
TURNS = [("straight bay", "NW", "P1", "W"), ("90 bay", "J", "P2", "Bay90"), ("hammerhead", "H", "D", "Hammer")]
BALLOON_REACH, BALLOON_HALF = 2700, 510
RETURN_ROAD_Y, FLOOR_MIN_Y = -4000, -17000
WIDE = 900  # approximate Wide service road width, uu

fig, ax = plt.subplots(figsize=(14, 7))
for a, b in ROADS:
    (x0, y0), (x1, y1) = N[a], N[b]
    ax.plot([x0, x1], [y0, y1], color="#555", lw=WIDE / 100, solid_capstyle="butt", alpha=0.6)
for name, (dx, dy) in DEAD_ENDS.items():
    x, y = N[name]
    rx, ry = (BALLOON_REACH, BALLOON_HALF) if dx else (BALLOON_HALF, BALLOON_REACH)
    cx, cy = x + dx * BALLOON_REACH / 2, y + dy * BALLOON_REACH / 2
    ax.add_patch(plt.Rectangle((cx - rx / (2 if dx else 1), cy - ry / (1 if dx else 2)), rx if dx else 2 * rx,
                               2 * ry if dx else ry, fill=False, ls=":", color="#999"))
for label, node, pull, bay in TURNS:
    ax.annotate("", xy=N[bay], xytext=N[pull], arrowprops=dict(arrowstyle="->", color="#e8820c", lw=2,
                connectionstyle="arc3,rad=0.25" if label != "straight bay" else "arc3"))
    ax.text(*N[bay], "  " + label, color="#e8820c", fontsize=9)
for name, (x, y) in N.items():
    ax.plot(x, y, "o", color="k", ms=3)
    ax.text(x, y + 250, name, fontsize=7)
ax.axhline(RETURN_ROAD_Y, color="#2a7", lw=2, label="loop course return road (Y -4000)")
ax.axhline(FLOOR_MIN_Y, color="#a33", lw=1, ls="--", label="floor edge (Y -17000)")
ax.set_aspect("equal")
ax.invert_yaxis()  # UE top view: +Y down the screen
ax.set_title("M_RigTest reversing yard, to scale (uu); orange = reverse turns, dotted = bowser balloons")
ax.legend(loc="lower right", fontsize=8)
ax.grid(alpha=0.2)
out = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "superpowers", "plans", "2026-09-26-rig-yard.png")
fig.savefig(out, dpi=110, bbox_inches="tight")
print("wrote", os.path.abspath(out))
