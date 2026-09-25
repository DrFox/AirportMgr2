"""Authors a ground vehicle's AnimGraph from Saved/<key>_vehicle_plan.json, and reads it back.

  python Tools/wire_vehicle_anim.py <key>            # wires, compiles, saves, verifies
  python Tools/wire_vehicle_anim.py <key> --verify   # verifies only, changes nothing
  python Tools/wire_vehicle_anim.py <key> --read     # prints the chain as it stands

  keys: fueltruck1 catering1 baggageCart1 curtainTrailer1

THE EDITOR MUST BE RUNNING; the plan comes from Tools/Python/build_vehicle_anims.py, which
needs it CLOSED. A worktree's editor answers on another port - set AIRSIDE_MCP_PORT (see
Tools/Mcp.py).

WHY NOT wire_anim_lib.Model, and what is reused from it. wire_anim_lib wires ONE kind of row -
a rotation off a MakeRotator - onto a reference-pose chain, and takes its multipliers typed in
each model's script. A vehicle needs two more things: a TRANSLATION row (catering1's platform
slides; see UAirsideAgentAnim::PlatformOffsetUu) and a BASE POSE that is not the reference pose
(catering1's Lift clip, evaluated at LiftClipTimeSeconds, under everything else). And the
multipliers are MEASURED per bone by build_vehicle_anims.py, not typed. The MCP session, the
call wrapper, the save check and the timeout are wire_anim_lib's and are imported, not copied.

THE ORDER THE GRAPH RUNS IN, source first:
  [Lift clip at LiftClipTimeSeconds -> LocalToComponent]   catering1 only
  one Transform (Modify) Bone per plan row, in plan order  (a bone after every bone it parents)
  ComponentToLocal -> Output Pose
The clip goes FIRST because it poses box_lift, and platform and beacon ride on box_lift: a bone
node that ran before the clip would be overwritten by it.

MODES, set on every node rather than left to its defaults (both were got wrong once on
ABP_Plane2): a rotation row adds in Bone Space and ignores translation; a translation row adds
in Bone Space and ignores rotation. Scale is ignored everywhere.

IDEMPOTENT BY DELETION, as every wiring script here is: all nodes but Output Pose go first.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Mcp  # noqa: E402
import wire_anim_lib  # noqa: E402

SAVED = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Saved")

ROTATE_MODES = {
    "translationMode": "BMM_Ignore",
    "rotationMode": "BMM_Additive",
    "scaleMode": "BMM_Ignore",
    "rotationSpace": "BCS_BoneSpace",
}
TRANSLATE_MODES = {
    "translationMode": "BMM_Additive",
    "rotationMode": "BMM_Ignore",
    "scaleMode": "BMM_Ignore",
    "translationSpace": "BCS_BoneSpace",
}

WIRE = r'''
import json
BP = %(bp)s
G = %(graph)s
ROWS = %(rows)s
LIFT = %(lift)s
ROT = %(rot)s
TRN = %(trn)s
BT = "editor_toolset.toolsets.blueprint.BlueprintTools."
OT = "editor_toolset.toolsets.object.ObjectTools."
AT = "editor_toolset.toolsets.asset.AssetTools."

def node(type_id, x, y):
    return execute_tool(BT + "create_node",
        json.dumps({"graph": G, "type_id": type_id, "pos": {"x": x, "y": y}}))["returnValue"]

def info(n):
    return execute_tool(BT + "get_node_infos", json.dumps({"nodes": [n]}))["returnValue"][0]

def pin_index(ni, side, name):
    for p in (ni["output_pins"] if side == "out" else ni["input_pins"]):
        if p["name"] == name:
            return p["pin_id"]["index_id"]
    raise RuntimeError("no %%s pin %%r on %%s" %% (side, name, ni["type_id"]))

def connect(a, ai, b, bi):
    execute_tool(BT + "connect_pins", json.dumps({
        "output_pin": {"direction": "EGPD_Output", "index_id": ai, "node": a},
        "input_pin":  {"direction": "EGPD_Input",  "index_id": bi, "node": b}}))

def driven(var, mul, x, y):
    """The variable, times -1 if the measured sign says so. Returns (node, output pin index)."""
    getter = node("Variables|Airside|Get" + var, x - 600, y)
    if mul > 0:
        return getter, 0
    # A PROMOTABLE operator: wildcard until A is connected, then re-read for B and the result.
    m = node("Utilities|Operators|Multiply", x - 300, y)
    connect(getter, 0, m, pin_index(info(m), "in", "A"))
    m_ni = info(m)
    execute_tool(BT + "set_pin_value", json.dumps({
        "pin": {"direction": "EGPD_Input", "index_id": pin_index(m_ni, "in", "B"), "node": m},
        "value": "%%f" %% mul}))
    return m, pin_index(m_ni, "out", "ReturnValue")

def run():
    root = execute_tool(BT + "find_nodes",
        json.dumps({"graph": G, "title": "Output Pose"}))["returnValue"][0]
    for n in execute_tool(BT + "find_nodes", json.dumps({"graph": G, "title": ""}))["returnValue"]:
        if n["refPath"] != root["refPath"]:
            execute_tool(BT + "delete_node", json.dumps({"node": n}))

    prev = None
    if LIFT:
        seq = node("Animation|Sequences|Evaluate'%%s'" %% LIFT, -2400, -400)
        t = node("Variables|Airside|GetLiftClipTimeSeconds", -2800, -300)
        connect(t, 0, seq, pin_index(info(seq), "in", "ExplicitTime"))
        l2c = node("Animation|ConvertSpaces|LocalToComponent", -2000, -400)
        connect(seq, 0, l2c, 0)
        prev = l2c

    for row, r in enumerate(ROWS):
        y = row * 320
        mb = node("Animation|SkeletalControls|Transform(Modify)Bone", -560, y)
        values = {"boneToModify": {"boneName": r["bone"]}}
        values.update(ROT if r["mode"] == "rotate" else TRN)
        execute_tool(OT + "set_properties",
                     json.dumps({"instance": mb, "values": json.dumps({"node": values})}))
        mb_ni = info(mb)
        if r["mode"] == "rotate":
            mk = node("Math|Rotator|MakeRotator", -900, y + 80)
            target = "Rotation"
        else:
            mk = node("Math|Vector|MakeVector", -900, y + 80)
            target = "Translation"
        mk_ni = info(mk)
        src, src_pin = driven(r["variable"], r["mul"], -900, y + 120)
        connect(src, src_pin, mk, pin_index(mk_ni, "in", r["pin"]))
        connect(mk, pin_index(mk_ni, "out", "ReturnValue"), mb, pin_index(mb_ni, "in", target))
        if prev is not None:
            connect(prev, 0, mb, pin_index(mb_ni, "in", "ComponentPose"))
        prev = mb

    c2l = node("Animation|ConvertSpaces|ComponentToLocal", -200, 0)
    connect(prev, 0, c2l, 0)
    connect(c2l, 0, root, 0)

    execute_tool(BT + "compile_blueprint", json.dumps({"blueprint": BP, "warnings_as_errors": False}))
    saved = execute_tool(AT + "save_assets", json.dumps({"asset_paths": ["%(asset)s"]}))
    return {"wired": [r["bone"] for r in ROWS], "saved": saved}
'''

BT = "editor_toolset.toolsets.blueprint.BlueprintTools"
OT = "editor_toolset.toolsets.object.ObjectTools"


def load_plan(key):
    path = os.path.join(SAVED, "%s_vehicle_plan.json" % key)
    if not os.path.exists(path):
        sys.exit("No %s.\nClose the editor and run Tools/Python/build_vehicle_anims.py as a "
                 "commandlet first - the axes and signs are measured there." % path)
    with open(path) as handle:
        return json.load(handle)


def refs(plan):
    abp = plan["abp"]
    name = abp.rsplit("/", 1)[-1]
    return {"refPath": "%s.%s" % (abp, name)}, {"refPath": "%s.%s:AnimGraph" % (abp, name)}


def read_chain(session, graph):
    """[(bone, modes, driver expression)] walked back from Output Pose, source first, and the
    base pose's type_id if the chain starts from something other than the reference pose."""
    nodes = wire_anim_lib.call(session, BT, "find_nodes", {"graph": graph, "title": ""})
    if not nodes:
        return [], None
    info = {n["node"]["refPath"]: n for n in
            wire_anim_lib.call(session, BT, "get_node_infos", {"nodes": nodes})}

    def source(pin):
        if not pin["connected_pins"]:
            return pin["value"]
        n = info[pin["connected_pins"][0]["node"]["refPath"]]
        inner = [source(p) for p in n["input_pins"]
                 if p["connected_pins"] or p["value"] not in ("", "(LinkID=-1,SourceLinkID=-1)")]
        return "%s(%s)" % (n["type_id"].split("|")[-1], ",".join(inner))

    current = [n for n in info.values() if n["type_id"] == "Misc.|OutputPose"][0]
    chain = []
    while True:
        pins = [p for p in current["input_pins"] if "Pose" in p["type_id"]]
        if not pins or not pins[0]["connected_pins"]:
            break
        current = info[pins[0]["connected_pins"][0]["node"]["refPath"]]
        chain.append(current)

    base = None
    rows = []
    for n in reversed(chain):
        if "SequenceEvaluator" in n["type_id"]:
            time = [p for p in n["input_pins"] if p["name"] == "ExplicitTime"][0]
            base = "%s @ %s" % (n["type_id"].split("|")[-1], source(time))
        if "Transform(Modify)Bone" not in n["type_id"]:
            continue
        props = json.loads(wire_anim_lib.call(session, OT, "get_properties",
                                              {"instance": n["node"], "properties": ["node"]}))["node"]
        rotation = [p for p in n["input_pins"] if p["name"] == "Rotation"][0]
        translation = [p for p in n["input_pins"] if p["name"] == "Translation"][0]
        driver = source(rotation) if rotation["connected_pins"] else source(translation)
        rows.append((props["boneToModify"]["boneName"], props, driver))
    return rows, base


def expected(row):
    driven = "Get%s()" % row["variable"] if row["mul"] > 0 else \
        "float*float(Get%s(),%f)" % (row["variable"], row["mul"])
    parts = ["0.0", "0.0", "0.0"]
    if row["mode"] == "rotate":
        parts[["Roll", "Pitch", "Yaw"].index(row["pin"])] = driven
        return "MakeRotator(%s)" % ",".join(parts)
    parts[["X", "Y", "Z"].index(row["pin"])] = driven
    return "MakeVector(%s)" % ",".join(parts)


def verify(session, plan, graph):
    rows, base = read_chain(session, graph)
    ok = True
    want_base = plan["lift_clip"]
    if want_base:
        clip = want_base.rsplit("/", 1)[-1]
        if base is None or clip not in base or "GetLiftClipTimeSeconds()" not in base:
            print("FAIL the chain does not start from %s at GetLiftClipTimeSeconds() - got %s"
                  % (clip, base))
            ok = False
        else:
            print("PASS base pose %s" % base)
    elif base is not None:
        print("FAIL an unexpected base pose %s" % base)
        ok = False

    if len(rows) != len(plan["rows"]):
        print("FAIL %d driven bones in the chain, the plan has %d" % (len(rows), len(plan["rows"])))
        ok = False
    for i, want in enumerate(plan["rows"]):
        if i >= len(rows):
            print("FAIL %-10s missing from the chain" % want["bone"])
            ok = False
            continue
        bone, props, driver = rows[i]
        modes = ROTATE_MODES if want["mode"] == "rotate" else TRANSLATE_MODES
        bad = [k for k, v in modes.items() if props.get(k) != v]
        line_ok = bone == want["bone"] and not bad and driver == expected(want)
        if bone != want["bone"]:
            print("FAIL position %d is %s, the plan says %s - CHAIN ORDER" % (i, bone, want["bone"]))
        if bad:
            print("FAIL %-10s modes %s" % (bone, {k: props.get(k) for k in bad}))
        if driver != expected(want):
            print("FAIL %-10s driver %s, wanted %s" % (bone, driver, expected(want)))
        if line_ok:
            print("PASS %-10s %s" % (bone, driver))
        ok = ok and line_ok
    print("VERIFY %s" % ("PASS" if ok else "FAIL"))
    return ok


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    key = argv[1]
    plan = load_plan(key)
    bp, graph = refs(plan)
    session = wire_anim_lib.connect_mcp(wire_anim_lib.Model(key, plan["abp"], []))

    if "--read" in argv:
        rows, base = read_chain(session, graph)
        print("base: %s" % base)
        for bone, _, driver in rows:
            print("%-10s %s" % (bone, driver))
        return 0

    if "--verify" not in argv:
        lift = plan["lift_clip"].rsplit("/", 1)[-1] if plan["lift_clip"] else None
        script = WIRE % {"bp": repr(bp), "graph": repr(graph), "rows": repr(plan["rows"]),
                         "lift": repr(lift), "rot": repr(ROTATE_MODES),
                         "trn": repr(TRANSLATE_MODES), "asset": plan["abp"]}
        result = wire_anim_lib.call(
            session, "editor_toolset.toolsets.programmatic.ProgrammaticToolset",
            "execute_tool_script", {"script": script}, timeout=wire_anim_lib.WIRE_TIMEOUT)
        wired = json.loads(result)
        print("wired: %s" % wired["wired"])
        if not wire_anim_lib.saved_ok(wired.get("saved")):
            print("FAIL wired and compiled, but save_assets returned %r - NOT on disk. Save All "
                  "in the editor, then re-run with --verify." % (wired.get("saved"),))
            return 1
    return 0 if verify(session, plan, graph) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
