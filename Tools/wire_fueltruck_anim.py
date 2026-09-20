"""Authors ABP_FuelTruck1's AnimGraph, and reads it back to prove it.

  python Tools/wire_fueltruck_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_fueltruck_anim.py --verify   # verifies only, changes nothing

THE EDITOR MUST BE RUNNING. That is why this lives in Tools/ and not Tools/Python/: every
script in there is a `-run=pythonscript` commandlet that needs the editor CLOSED, and putting
an MCP driver among them would invite exactly the wrong one. The two halves of a model's
Anim Blueprint are opposite that way - build_fueltruck_anim.py creates the asset with the
editor down, this wires it with the editor up - and that is one restart, not a wall.

WHY THIS EXISTS AT ALL: ABP_FuelTruck1 shipped with an AnimGraph containing ONE node, the
Output Pose, and nothing connected to it. UAirsideContent::VehicleAnimClass has pointed at it
the whole time, and that property's own doc comment describes the symptom - "Null leaves it in
its reference pose - a truck that slides along with its wheels held still". It was not null;
it was wired to a graph that did nothing, which looks identical and reads as configured. The
gap survived because nothing could see inside an AnimGraph cheaply. Now something can, so the
graph has a generator, and a generated asset with no generator is what
docs/2026-09-20-animgraph-authoring.md exists to stop recurring.

IDEMPOTENT BY DELETION: every node but Output Pose is removed before anything is created. A
half-finished run therefore costs nothing to clean up, and re-running after an export changes
the rig is the supported way to rebuild rather than a risk. A .uasset has no merge - see the
project's note on resolving .uasset conflicts by re-authoring - and this is that script.

THE THREE DECISIONS THIS SCRIPT OWNS
------------------------------------
ORDER: each steer bone comes AFTER the wheel it carries. FCSPose::SafeSetCSBoneTransforms
exists to "refresh any Children they have that has been previously converted to Component
Space", so rotating the parent last takes the already-rolled wheel with it. ABP_Plane1 and
ABP_Plane2 both ship this order. build_fueltruck_anim.py printed the REVERSE until
2026-09-20, when a graph authored to its instruction was diffed against the aeroplanes and
disagreed; the instruction was corrected, not the assets.

AXIS: Yaw, in Bone Space, for all six. Not an assumption - the bind orientations were
measured out of the two .glb files and they match bone for bone. plane1's nosewheel_steer
resolves to identity in component space and so do steer_FL and steer_FR; plane1's rolling
bones resolve to -90 deg about X and so do all four wheels here, give or take the 1.2 deg of
camber build_export.py gave them. The rigger orients each bone so that its own Z is the axis
it is meant to turn about, which is why every driven bone in this project takes Yaw.

MODES: Translation Ignore, Rotation ADD TO EXISTING, Scale Ignore, Rotation Space Bone Space,
set explicitly rather than left to the node's defaults. Both were got wrong once on
ABP_Plane2: Translation on Replace writes (0,0,0) and drops each wheel at the root, and
Rotation on Replace discards the bind-pose orientation - here, the camber - and points every
wheel the same way.

THE BEACON IS NOT DRIVEN and its absence from PLAN is deliberate. It is rigged and it would
want a steady spin, but every angle on UAirsideAgentAnim is derived from the agent's motion
and a beacon turns whether or not the truck is moving. build_fueltruck_anim.py reports it
UNRECOGNISED for the same reason: a gap someone can see beats one that was quietly skipped.
"""
import json
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Mcp

BP_PATH = "/Game/Vehicles/FuelTruck1/ABP_FuelTruck1"
BP = {"refPath": "%s.ABP_FuelTruck1" % BP_PATH}
GRAPH = {"refPath": "%s.ABP_FuelTruck1:AnimGraph" % BP_PATH}

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
# The -1 matches every rolling bone in the fleet: UAirsideAgentAnim accumulates an absolute
# angle and the rig turns the other way about its own Z.
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

# Sent to ProgrammaticToolset as ONE call. Doing it from here instead would be ~90 MCP round
# trips; the sandbox can call any registered tool, so the whole graph costs one.
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
        # The getter's CATEGORY is the C++ UPROPERTY Category, so these are Variables|Airside|
        # and not Variables|Default|. Composing the type_id by hand returns "does not exist".
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
            # A PROMOTABLE operator - wildcard until something is plugged in, and it reads
            # back afterwards as Math|Float|float*float, which is NOT a type_id that creates
            # one. Connect A first, then re-read: B and ReturnValue move once it is a float.
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

    # The ModifyBone chain is component space; the Output Pose wants local.
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
                       "clientInfo": {"name": "wire_fueltruck_anim", "version": "1"}},
        })
    except OSError as exc:
        sys.exit("The editor is not running, or its MCP server is not up: %s" % exc)
    Mcp.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, session)
    return session


def call(session, toolset, tool, args):
    result = Mcp.rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": {
        "toolset_name": toolset, "tool_name": tool, "arguments": args}})
    text = result["content"][0]["text"]
    try:
        return json.loads(text)["returnValue"]
    except (ValueError, KeyError):
        # A tool that fails returns PROSE where a result would be JSON, and a traceback out
        # of execute_tool_script arrives the same way. Surface it rather than dying on the
        # decode: the message is the diagnosis and json.loads throws it away.
        sys.exit("%s.%s failed:\n%s" % (toolset.split(".")[-1], tool, text))


BT = "editor_toolset.toolsets.blueprint.BlueprintTools"
OT = "editor_toolset.toolsets.object.ObjectTools"


def read_chain(session):
    """Every driven bone, in chain order, walked back from Output Pose.

    ORDER IS READ, not assumed: it is one of the three decisions above, and a verifier that
    only checked membership would pass a graph with the steer bones in the wrong place.
    """
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
    for n in reversed(chain):          # reversed: report source-first, as PLAN reads
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
    """Fails loudly, bone by bone. Returns True only if the graph matches PLAN exactly."""
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
        # repr, NOT json.dumps. The result is pasted into a PYTHON source file, and
        # json.dumps writes None as `null`, which the sandbox rejects with "name 'null' is
        # not defined" - a runtime error from a script that parses perfectly.
        script = WIRE % {"bp": repr(BP), "graph": repr(GRAPH),
                         "plan": repr(PLAN), "modes": repr(MODES),
                         "asset": BP_PATH}
        result = call(session, "editor_toolset.toolsets.programmatic.ProgrammaticToolset",
                      "execute_tool_script", {"script": script})
        print("wired: %s" % json.loads(result)["wired"])
    sys.exit(0 if verify(session) else 1)


if __name__ == "__main__":
    main(sys.argv)
