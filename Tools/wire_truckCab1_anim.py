"""Authors ABP_TruckCab1's AnimGraph, and reads it back to prove it.

  python Tools/wire_truckCab1_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_truckCab1_anim.py --verify   # verifies only, changes nothing

THE EDITOR MUST BE RUNNING. Copied from Tools/wire_fueltruck_anim.py (see
docs/2026-09-20-animgraph-authoring.md) - same mechanism, this model's bones.

AXIS: Yaw, in Bone Space, for all six - MEASURED, not assumed (CLAUDE.md: "measured beats
typed"; also this project's own later correction of the Yaw-by-habit shortcut, 2026-09-21).
Read off SK_TruckCab1's reference pose via airside_anim.bone_frames on 2026-09-24: each wheel
bone's local Z (its Yaw axis) resolves to UE's Y (the axle direction) at -1.0000, and each
steer bone's local frame is the IDENTITY (X,Y,Z exactly world-aligned), so its local Z is the
vertical kingpin axis. Both are the SAME shape SK_FuelTruck1 measures to, bone for bone (see
the measurement's own log) - the rigger oriented every driven bone so its own Z is the axis it
turns about, exactly as wire_fueltruck_anim.py's header already argues for that rig.

SIGN: -1.0 on every wheel, none on steer - copied from wire_fueltruck_anim.py's own figures
because the geometry is bone-for-bone identical (same -1.0000 alignment, same identity steer
frame), not because "the fleet does it that way" alone.

ORDER: each steer bone AFTER the wheel it carries (steer_FL after wheel_FL, steer_FR after
wheel_FR) - FCSPose::SafeSetCSBoneTransforms refreshes an already-converted child, so the
parent's steering carries the rolled wheel with it. Matches ABP_Plane1/ABP_Plane2 and
ABP_FuelTruck1 (corrected 2026-09-20).

MODES: Translation Ignore, Rotation ADD TO EXISTING, Scale Ignore, Rotation Space Bone Space -
set explicitly, for the same reasons wire_fueltruck_anim.py gives (Translation on Replace
detaches the wheel; Rotation on Replace discards the bind-pose orientation, here the camber
build_export.py's rigger comment records).

fifth_wheel IS NOT ON THIS PLAN. It is the kingpin coupling socket the trailer attaches
through - see build_rig_anim.py's header for the substring trap it would otherwise fall into
in a NAME-based bone plan (this script's PLAN is hand-written, not generated, so the trap does
not apply here - it is omitted because nothing drives it, not because it was filtered).
"""
import json
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Mcp

BP_PATH = "/Game/Vehicles/Rig/TruckCab1/ABP_TruckCab1"
BP = {"refPath": "%s.ABP_TruckCab1" % BP_PATH}
GRAPH = {"refPath": "%s.ABP_TruckCab1:AnimGraph" % BP_PATH}

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
PLAN = [
    ("wheel_RL", "WheelAngleDegrees", -1.0),
    ("wheel_RR", "WheelAngleDegrees", -1.0),
    ("wheel_FL", "WheelAngleDegrees", -1.0),
    ("wheel_FR", "WheelAngleDegrees", -1.0),
    ("steer_FL", "SteerAngleDegrees", None),
    ("steer_FR", "SteerAngleDegrees", None),
]

MODES = {
    "translationMode": "BMM_Ignore",
    "rotationMode": "BMM_Additive",
    "scaleMode": "BMM_Ignore",
    "rotationSpace": "BCS_BoneSpace",
}

# Sent to ProgrammaticToolset as ONE call - see wire_fueltruck_anim.py's own note on why.
WIRE = r'''
import json
BP = %(bp)s
G = %(graph)s
PLAN = %(plan)s
MODES = %(modes)s
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

def run():
    root = execute_tool(BT + "find_nodes",
        json.dumps({"graph": G, "title": "Output Pose"}))["returnValue"][0]
    for n in execute_tool(BT + "find_nodes", json.dumps({"graph": G, "title": ""}))["returnValue"]:
        if n["refPath"] != root["refPath"]:
            execute_tool(BT + "delete_node", json.dumps({"node": n}))

    prev = None
    for row, (bone, var, mul) in enumerate(PLAN):
        y = row * 320
        getter = node("Variables|Airside|Get" + var, -1500, y + 120)
        mk = node("Math|Rotator|MakeRotator", -900, y + 80)
        mb = node("Animation|SkeletalControls|Transform(Modify)Bone", -560, y)

        values = {"boneToModify": {"boneName": bone}}
        values.update(MODES)
        execute_tool(OT + "set_properties",
                     json.dumps({"instance": mb, "values": json.dumps({"node": values})}))

        mk_ni, mb_ni = info(mk), info(mb)
        yaw = pin_index(mk_ni, "in", "Yaw")
        if mul is None:
            connect(getter, 0, mk, yaw)
        else:
            m = node("Utilities|Operators|Multiply", -1200, y + 120)
            connect(getter, 0, m, pin_index(info(m), "in", "A"))
            m_ni = info(m)
            execute_tool(BT + "set_pin_value", json.dumps({
                "pin": {"direction": "EGPD_Input", "index_id": pin_index(m_ni, "in", "B"),
                        "node": m}, "value": "%%f" %% mul}))
            connect(m, pin_index(m_ni, "out", "ReturnValue"), mk, yaw)

        connect(mk, pin_index(mk_ni, "out", "ReturnValue"), mb, pin_index(mb_ni, "in", "Rotation"))
        if prev is not None:
            connect(prev, 0, mb, pin_index(mb_ni, "in", "ComponentPose"))
        prev = mb

    c2l = node("Animation|ConvertSpaces|ComponentToLocal", -200, 0)
    connect(prev, 0, c2l, 0)
    connect(c2l, 0, root, 0)

    execute_tool(BT + "compile_blueprint", json.dumps({"blueprint": BP, "warnings_as_errors": False}))
    execute_tool(AT + "save_assets", json.dumps({"asset_paths": ["%(asset)s"]}))
    return {"wired": [b for b, _, _ in PLAN]}
'''


def connect_mcp():
    try:
        session, _ = Mcp.post({
            "jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                       "clientInfo": {"name": "wire_truckCab1_anim", "version": "1"}},
        })
    except OSError as exc:
        sys.exit("The editor is not running, or its MCP server is not up: %s" % exc)
    Mcp.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, session)
    return session


def call(session, toolset, tool, args, timeout=Mcp.DEFAULT_TIMEOUT):
    result = Mcp.rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": {
        "toolset_name": toolset, "tool_name": tool, "arguments": args}}, timeout=timeout)
    text = result["content"][0]["text"]
    try:
        return json.loads(text)["returnValue"]
    except (ValueError, KeyError):
        sys.exit("%s.%s failed:\n%s" % (toolset.split(".")[-1], tool, text))


BT = "editor_toolset.toolsets.blueprint.BlueprintTools"
OT = "editor_toolset.toolsets.object.ObjectTools"


def read_chain(session):
    nodes = call(session, BT, "find_nodes", {"graph": GRAPH, "title": ""})
    if not nodes:
        return []
    info = {n["node"]["refPath"]: n for n in
            call(session, BT, "get_node_infos", {"nodes": nodes})}

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
        pin = current["input_pins"][0]
        if not pin["connected_pins"]:
            break
        current = info[pin["connected_pins"][0]["node"]["refPath"]]
        chain.append(current)

    rows = []
    for n in reversed(chain):
        if "Transform(Modify)Bone" not in n["type_id"]:
            continue
        props = json.loads(call(session, OT, "get_properties",
                                {"instance": n["node"], "properties": ["node"]}))["node"]
        rotation = [p for p in n["input_pins"] if p["name"] == "Rotation"][0]
        rows.append((props["boneToModify"]["boneName"],
                     {k: props[k] for k in MODES},
                     source(rotation)))
    return rows


def verify(session):
    rows = read_chain(session)
    ok = True

    if len(rows) != len(PLAN):
        print("FAIL %d driven bones in the chain, PLAN has %d" % (len(rows), len(PLAN)))
        ok = False

    for i, (bone, variable, mul) in enumerate(PLAN):
        if i >= len(rows):
            print("FAIL %-9s missing from the chain" % bone)
            ok = False
            continue
        got_bone, got_modes, got_rotation = rows[i]
        want = "MakeRotator(0.0,0.0,Get%s())" % variable if mul is None else \
               "MakeRotator(0.0,0.0,float*float(Get%s(),%f))" % (variable, mul)
        if got_bone != bone:
            print("FAIL position %d is %s, PLAN says %s - the CHAIN ORDER is wrong"
                  % (i, got_bone, bone))
            ok = False
        if got_modes != MODES:
            print("FAIL %-9s modes %s" % (got_bone, got_modes))
            ok = False
        if got_rotation != want:
            print("FAIL %-9s rotation %s, wanted %s" % (got_bone, got_rotation, want))
            ok = False
        if got_bone == bone and got_modes == MODES and got_rotation == want:
            print("PASS %-9s %s" % (bone, got_rotation))

    print("VERIFY %s" % ("PASS" if ok else "FAIL"))
    return ok


def main(argv):
    session = connect_mcp()
    if "--verify" not in argv:
        script = WIRE % {"bp": repr(BP), "graph": repr(GRAPH),
                         "plan": repr(PLAN), "modes": repr(MODES),
                         "asset": BP_PATH}
        result = call(session, "editor_toolset.toolsets.programmatic.ProgrammaticToolset",
                      "execute_tool_script", {"script": script}, timeout=120)
        print("wired: %s" % json.loads(result)["wired"])
    sys.exit(0 if verify(session) else 1)


if __name__ == "__main__":
    main(sys.argv)
