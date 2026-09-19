"""The palette, in one place, and the sRGB -> linear conversion every authoring script needs.

Spec `docs/superpowers/specs/2026-09-12-environment-art-direction-design.md` section 1.1 is
the source of truth for these values; this module is the source of truth for what the
MATERIALS get, and the two must agree. Hex is kept verbatim so the two can be diffed by eye
- a converted float typed into a script is a swatch that has already drifted from its table.

Imported rather than copied, because before this existed the surface colours were literals
at three call sites (`build_runway_materials.py` tints, `build_kerb_material.py`,
`build_road_material.py`) and no two of them could be checked against each other.
"""


def srgb(hexcode):
    """sRGB hex -> linear (r, g, b) floats. Returns a tuple, not an FLinearColor, so this
    module stays importable outside the editor - which is what lets it be unit-checked."""
    h = hexcode.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i:i + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return tuple(out)


def linear_color(hexcode):
    """srgb() as an unreal.LinearColor. Editor-only; imported lazily so the module itself
    does not require the engine."""
    import unreal
    r, g, b = srgb(hexcode)
    return unreal.LinearColor(r, g, b, 1.0)


# --- Hard surface (spec 1.1, added 2026-09-19) ------------------------------------------
# Runway asphalt is darker than taxiway asphalt on purpose: it is laid and maintained
# differently and genuinely reads darker, and the contrast is functional from the build
# camera - it says which surface an aircraft is on before any marking is legible.
RUNWAY_ASPHALT = "#343B40"
TAXIWAY_ASPHALT = "#454D50"
APRON_CONCRETE = "#9C9B91"
CONCRETE_HIGHLIGHT = "#B8B7AC"

# --- Terrain & ground (sampled from the concept sheet) -----------------------------------
GRASS_MOWN = "#7D8E47"
GRASS_ROUGH = "#748546"
DIRT = "#C6B283"
DRY_STRAW = "#B6A66A"

# --- Markings ----------------------------------------------------------------------------
MARKING_YELLOW = "#F6D857"
MARKING_WHITE = "#E8E8E4"
MARKING_RED = "#C53033"
