"""The diorama edge (2026-10-02) - owned land cut as a vertical slice, on a test map. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Run build_ground_material.py FIRST: it is what wires the owned-rect clip into M_Ground.

Builds M_Diorama, a copy of M_Test_Small, with:
- the Landscape on MI_Ground_Diorama: MI_Ground's child with the blend mode overridden to
  Masked, so it honours the clip (M_Ground itself stays Opaque, so no other map changes);
- the airport's starting land (FLandGrid on its network: 600 m tiles, 8x8, a 1x2 at the bottom centre),
  authored through one AAirsideOwnedLandActor, which then DRAWS it - writes the clip (MPC_OwnedLand) and lays
  the strata walls. The camera, the grass and every build refusal read the same grid;
- the content set's owned-land rows (collection, wall mesh, M_DioramaStrata's instance);
- NO backdrop. A first pass put an unlit plane 600 m under the plinth so the void would be a
  chosen colour rather than the SkyAtmosphere's navy ground (env spec section 10, Slice D). Set
  to pure red it never showed: the exponential height fog thickens below the ground and fills
  everything under the plinth with the sky's own colour (2026-10-02 screenshots). That reads as
  a diorama floating in sky, for free, so the plane went. Revisit only if a "table" look is wanted.

Everything is a first guess to be judged on a 20/150/600 m screenshot. The tunables live on
MI_DioramaStrata / MI_Ground_Diorama so they can be changed live.

The start tiles are centred on the road network's footprint (plus MARGIN_M), once. After that the land
is the airport's: the Buy land tool grows it, and the level's saved network carries it.

Every result line is prefixed DIORAMA: so it can be grepped out of the log.
"""
import os
import sys
import unreal

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import airside_matnodes as nodes
import airside_palette as palette

MARKER = "DIORAMA:"
SOURCE_MAP = "/Game/Maps/M_Test_Small"
MAP = "/Game/Maps/M_Diorama"
OUT_DIR = "/Game/Environment/Diorama"
GROUND_MI = "/Game/Environment/MI_Ground"
TAG = "DioramaPrototype"

# Owned land: the road network's footprint plus this margin each side, in metres. If the road
# actor reports no bounds headlessly (its mesh may be built only at runtime), the fallback is a
# square of FALLBACK_HALF metres about the landscape's centre.
MARGIN_M = 60.0
FALLBACK_HALF_M = 600.0

# Plinth depth below the ground, metres. Exaggerated on purpose - a diorama base, not geology.
DEPTH_M = 40.0

# The land grid (land purchase spec R1-R3): 600 m tiles, 8x8, a 1x2 start at the bottom centre.
TILE = 60000.0
COLUMNS, ROWS = 8, 8
START = [(0, 3), (0, 4)]
WALL_THICK_M = 1.0
# Fraction of the strata colour added as emissive. The walls facing away from the sun are lit
# only by a blue sky and fog, and at 0 they rendered as navy slabs with the bands invisible
# (2026-10-02, a_150.png). A diorama shows its cut on every side, so the bands must survive shade.
STRATA_LIFT = 0.25

# Strata, top down: (param, hex, thickness m). Colours are first guesses; GRASS takes the palette.
STRATA = [
    ("Grass", palette.GRASS_MOWN, 0.6),
    ("Topsoil", "#6B4A30", 1.6),
    ("Clay", "#9A6A43", 6.0),
    ("Sand", palette.DIRT, 5.0),
    ("Rock", "#6E6A66", 0.0),   # everything below
]

CUBE = "/Engine/BasicShapes/Cube.Cube"


def say(msg):
    unreal.log("%s %s" % (MARKER, msg))


def fail(msg):
    unreal.log_error("%s FAIL %s" % (MARKER, msg))


lib = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def material(name, rebuild):
    path = "%s/%s" % (OUT_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        nodes.clear_graph(lib, mat, on_fail=fail)
    else:
        mat = tools.create_asset(name, OUT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    rebuild(mat)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("built %s" % path)
    return mat


def instance(name, parent):
    """Created only if absent, so values tuned live survive a re-run (the MI_Ground rule)."""
    path = "%s/%s" % (OUT_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
    else:
        mi = tools.create_asset(name, OUT_DIR, unreal.MaterialInstanceConstant,
                                unreal.MaterialInstanceConstantFactoryNew())
    lib.set_material_instance_parent(mi, parent)
    return mi


def build_strata(mat):
    """Bands by depth below StrataTop, with a slow sideways wobble so the layers read as
    deposited rather than ruled. World Z, not UV, so the four walls line up at the corners."""
    custom = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    names = ["WP", "Top", "Wobble"] + [s[0] for s in STRATA] + ["%sDepth" % s[0] for s in STRATA[:-1]]
    pins = []
    for n in names:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", n)
        pins.append(pin)
    custom.set_editor_property("inputs", pins)
    code = [
        "float d = (Top - WP.z) / 100.0;",
        "d += Wobble * (sin(WP.x * 0.0011 + WP.y * 0.0007) * 0.6 + sin(WP.x * 0.0031 - WP.y * 0.0023) * 0.4);",
        "float edge = 0.0;",
    ]
    acc = []
    for s in STRATA[:-1]:
        acc.append("%sDepth" % s[0])
        code.append("if (d < %s) return %s;" % (" + ".join(acc), s[0]))
    code.append("return %s;" % STRATA[-1][0])
    custom.set_editor_property("code", "\n".join(code))

    ok = True
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -700, -200)
    ok &= lib.connect_material_expressions(world, "", custom, "WP")
    ok &= lib.connect_material_expressions(nodes.scalar(lib, mat, "StrataTop", 0.0, -700, -140), "", custom, "Top")
    ok &= lib.connect_material_expressions(nodes.scalar(lib, mat, "StrataWobble", 0.5, -700, -80), "", custom, "Wobble")
    y = 0
    for name, hexcode, thick in STRATA:
        ok &= lib.connect_material_expressions(nodes.vector(lib, mat, name, hexcode, -700, y), "", custom, name)
        y += 80
        if thick:
            ok &= lib.connect_material_expressions(
                nodes.scalar(lib, mat, "%sDepth" % name, thick, -700, y), "", custom, "%sDepth" % name)
            y += 60
    if not ok:
        fail("strata pins did not all connect")
    lib.connect_material_property(custom, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lift = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -100, 150)
    lib.connect_material_expressions(custom, "", lift, "A")
    lib.connect_material_expressions(nodes.scalar(lib, mat, "StrataLift", STRATA_LIFT, -300, 200), "", lift, "B")
    lib.connect_material_property(lift, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.connect_material_property(nodes.scalar(lib, mat, "Roughness", 0.95, -300, 300), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)


def ground_instance():
    mi = instance("MI_Ground_Diorama", unreal.EditorAssetLibrary.load_asset(GROUND_MI))
    overrides = mi.get_editor_property("base_property_overrides")
    overrides.set_editor_property("override_blend_mode", True)
    overrides.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mi.set_editor_property("base_property_overrides", overrides)
    return mi


def ensure_map():
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        say("%s exists" % MAP)
        return
    # duplicate_asset, never a byte copy: a copied .umap keeps its internal world name.
    if not unreal.EditorAssetLibrary.duplicate_asset(SOURCE_MAP, MAP):
        fail("duplicate %s -> %s" % (SOURCE_MAP, MAP))
    say("duplicated %s -> %s" % (SOURCE_MAP, MAP))


def owned_rect(all_actors, landscape):
    road = [a for a in all_actors if a.get_class().get_name() == "RoadNetworkActor"]
    if road:
        origin, extent = road[0].get_actor_bounds(False)
        if extent.x > 1000 and extent.y > 1000:
            m = MARGIN_M * 100
            say("rect from road bounds origin=%s extent=%s" % (origin, extent))
            return (origin.x - extent.x - m, origin.y - extent.y - m,
                    origin.x + extent.x + m, origin.y + extent.y + m)
        say("road bounds empty (extent=%s) - falling back" % extent)
    origin, extent = landscape.get_actor_bounds(False)
    h = FALLBACK_HALF_M * 100
    return (origin.x - h, origin.y - h, origin.x + h, origin.y + h)


CONTENT_SET = "/Game/DA_AirsideContent"


def wire_content(mi_strata):
    """The owned-land rows of the content set - AAirsideOwnedLandActor resolves its collection,
    wall mesh and wall material through UAirsideSettings::ResolveOwnedLandKit, never by path."""
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        fail("no content set at %s" % CONTENT_SET)
        return False
    mpc = unreal.EditorAssetLibrary.load_asset(nodes.OWNED_LAND_MPC)
    if mpc is None:
        fail("%s missing - run build_ground_material.py first" % nodes.OWNED_LAND_MPC)
        return False
    content.set_editor_property("owned_land_collection", mpc)
    content.set_editor_property("plinth_wall_mesh", unreal.EditorAssetLibrary.load_asset(CUBE))
    content.set_editor_property("plinth_wall_material", mi_strata)
    unreal.EditorAssetLibrary.save_loaded_asset(content, only_if_is_dirty=False)
    say("content set wired: %s, %s, %s" % (mpc.get_name(), CUBE, mi_strata.get_name()))
    return True


def run():
    strata = material("M_DioramaStrata", build_strata)
    mi_strata = instance("MI_DioramaStrata", strata)
    mi_ground = ground_instance()
    if not wire_content(mi_strata):
        return

    ensure_map()
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(MAP)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    all_actors = actors.get_all_level_actors()

    # The first pass's loose wall cubes (tagged), and any earlier owned-land actor: re-run safe.
    for a in all_actors:
        if TAG in [str(t) for t in a.tags] or isinstance(a, unreal.AirsideOwnedLandActor):
            actors.destroy_actor(a)
    all_actors = actors.get_all_level_actors()

    lands = [a for a in all_actors if isinstance(a, unreal.Landscape)]
    if len(lands) != 1:
        fail("expected 1 Landscape, found %d" % len(lands))
        return
    land = lands[0]
    # The actor's Z, NOT the bounds' max: a flat landscape's bounds carry 1 m of headroom, and the
    # first pass used it - walls stood 95 cm proud of a ground that a trace put at Z=0.
    top = land.get_actor_location().z
    say("ground Z %.1f" % top)

    x0, y0, x1, y1 = owned_rect(all_actors, land)
    say("owned rect cm: %.0f %.0f %.0f %.0f (%.0f x %.0f m)" % (x0, y0, x1, y1, (x1 - x0) / 100, (y1 - y0) / 100))

    lib.set_material_instance_scalar_parameter_value(mi_strata, "StrataTop", top)
    for mi in (mi_ground, mi_strata):
        unreal.EditorAssetLibrary.save_loaded_asset(mi, only_if_is_dirty=False)
    land.set_editor_property("landscape_material", mi_ground)

    edge = actors.spawn_actor_from_class(unreal.AirsideOwnedLandActor, unreal.Vector(0, 0, top))
    edge.set_actor_label("OwnedLand")
    edge.set_editor_property("plinth_depth", DEPTH_M * 100)
    edge.set_editor_property("wall_thickness", WALL_THICK_M * 100)
    # Last, through the UFUNCTION: it lays the walls and writes the clip. A bare property write
    # reruns no construction headlessly.
    # THE LAND LIVES ON THE AIRPORT (land purchase spec 2); the edge authors it once and draws it. R3: the 1x2 start
    # sits at the bottom centre - column 0 is the default camera's bottom edge (it looks along +X), rows 3-4 the
    # middle of eight - centred on the road network's footprint.
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    origin = unreal.Vector2D(cx - 0.5 * TILE, cy - 4.0 * TILE)
    edge.author_starting_land(origin, TILE, COLUMNS, ROWS, [unreal.IntPoint(c, r) for c, r in START])

    if not levels.save_current_level():
        fail("save_current_level returned False")
        return
    # Read it back: a save can report success and write nothing (unreal-editing-levels-headlessly).
    levels.load_level(MAP)
    after = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    edges = [a for a in after if isinstance(a, unreal.AirsideOwnedLandActor)]
    loose = [a for a in after if TAG in [str(t) for t in a.tags]]
    if len(edges) == 1 and not loose:
        e = edges[0]
        say("PASS one owned-land actor after reload; grep the log for 'OwnedLand: 2 tile(s), 4 wall run(s)'")
    else:
        fail("%d owned-land actors and %d loose walls after reload" % (len(edges), len(loose)))
    land = [a for a in after if isinstance(a, unreal.Landscape)][0]
    lm = land.get_editor_property("landscape_material")
    say("landscape material after reload: %s" % (lm.get_path_name() if lm else None))
    say("DONE")


run()
