"""The mechanism behind every Tools/wire_<model>_anim.py: authors an AnimGraph and reads it
back to prove it. A model's own script holds its bones and its paths and calls main() here.

THE EDITOR MUST BE RUNNING. That is why this lives in Tools/ and not Tools/Python/: every
script in there is a `-run=pythonscript` commandlet that needs the editor CLOSED. The two
halves of a model's Anim Blueprint are opposite that way - Tools/Python/build_<model>_anim.py
creates the asset and resolves the rotation axes with the editor down, the wiring script wires
it with the editor up - and that is one restart, not a wall. See
docs/2026-09-20-animgraph-authoring.md.

EXTRACTED FROM wire_plane5_anim.py ON 2026-09-21, when plane7 needed it. That script was
itself copied from wire_fueltruck_anim.py, so plane7 would have been the third near-identical
400 lines - and on the same day airside_anim.py was made out of four copies of the bone plan,
a third copy of this was not defensible. Nothing here changed in the move except that five
constants became a Model.

wire_fueltruck_anim.py is deliberately NOT converted. It predates the measured axis plan and
hard-codes Yaw for every bone, so folding it in would mean changing the tooling of a shipped,
hand-verified asset for tidiness alone - and it is the worked example
docs/2026-09-20-animgraph-authoring.md points at, which is a reason to leave it legible
standing on its own.

THE PLAN USED TO BE HALF READ FROM A FILE; NOW IT IS WHOLLY READ FROM ONE
--------------------------------------------------------------------------
Until Issue #294, a Model's plan (bone, variable, multiplier) was typed by hand into each
Tools/wire_plane<N>_anim.py, and only the rotator component and sign - facts about the
IMPORTED SKELETON this script cannot see - came from Saved/<model>_axis_plan.json.
Tools/Python/build_aircraft_anim.py now derives the bone/variable/multiplier columns too (see
airside_anim.driven_bone_plan() and anim_multiplier_for()) and writes the WHOLE row into that
file, so Tools/wire_plane_anim.py - the twelve wrapper scripts' replacement - carries no
per-aircraft table at all; load_axis_plan() below fills `model.plan` from it. This refuses to
run without that file.

WHY THAT IS WORTH THE INDIRECTION. Every wiring script before plane5 hard-codes "Yaw, in Bone
Space" for every bone, on the strength of wire_fueltruck_anim.py's note that "the rigger
orients each bone so that its own Z is the axis it is meant to turn about". That note is a
true observation about plane1's and the fuel truck's rigs. It is not a law, nothing enforces
it, and plane5 was the first rig with FOUR doors and THREE legs folding two different ways -
the first rig where a single wrong axis would be easy to miss on screen and impossible to spot
in a diff. Measured beats typed; see CLAUDE.md.

THE FOUR DECISIONS THIS SCRIPT OWNS
-----------------------------------
ORDER: a bone comes AFTER every bone it is the PARENT of. FCSPose::SafeSetCSBoneTransforms
exists to "refresh any Children they have that has been previously converted to Component
Space", so rotating the parent last takes the already-rotated child with it. ABP_Plane1 and
ABP_Plane2 both ship that order for nosewheel_steer. Since Issue #294 the order is DERIVED,
not typed - see driven_bone_plan()'s own docstring for how reversing the .glb's own joint
order always yields a valid one.

  THE RULE BINDS TWICE ON A RETRACTING RIG. plane1 and the fuel truck have one parent/child
  pair each (steer over wheel). plane5 and plane7 have a three-deep chain -
  gear_nose > nosewheel_steer > nosewheel - AND gear_L/gear_R as the parents of
  wheel_L/wheel_R. Retract before roll and the wheels spin in the bay instead of with it.

AXIS: read, per bone, from Saved/<model>_axis_plan.json. See above.

MODES: Translation Ignore, Rotation ADD TO EXISTING, Scale Ignore, Rotation Space Bone Space,
set explicitly rather than left to the node's defaults. Both were got wrong once on
ABP_Plane2: Translation on Replace writes (0,0,0) and drops each part at the root, and
Rotation on Replace discards the bind-pose orientation.

ONE DOOR ANGLE DRIVES FOUR DOORS, and nothing here mirrors anything. door_main_L/_R close
OUTBOARD and door_nose_L/_R close INBOARD; the mirroring lives in each bone's rest direction,
which is what the measured axis column comes back carrying. A script that "helpfully" negated
the right-hand side would break exactly the rig the rigger got right.

IDEMPOTENT BY DELETION: every node but Output Pose is removed before anything is created. A
half-finished run costs nothing to clean up, and re-running after an export changes the rig
is the supported way to rebuild rather than a risk. A .uasset has no merge - see the
project's note on resolving .uasset conflicts by re-authoring - and this is that script.
"""
import json
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Mcp

class Model(object):
    """Everything a wiring run needs that is not mechanism.

    key       the models-repo folder name; names Saved/<key>_axis_plan.json and the client
    bp_path   the Anim Blueprint's package path
    plan      (bone, variable, multiplier) in CHAIN ORDER, source first; None drives directly.
              NOT PASSED IN, since Issue #294 - load_axis_plan() fills it from
              Saved/<key>_axis_plan.json, which Tools/Python/build_aircraft_anim.py now writes
              whole (bone, variable AND multiplier, not only the component and sign it used
              to). `plan` is still accepted as a constructor argument, kept for
              Tools/wire_vehicle_anim.py's throwaway Model(key, bp_path, []) - it never calls
              load_axis_plan on that instance - and is otherwise unused; the fleet's
              wire_plane_anim.py callers pass only key and bp_path.
    """

    def __init__(self, key, bp_path, plan=None):
        self.key = key
        self.bp_path = bp_path
        self.plan = plan
        name = bp_path.rsplit("/", 1)[-1]
        self.bp = {"refPath": "%s.%s" % (bp_path, name)}
        self.graph = {"refPath": "%s.%s:AnimGraph" % (bp_path, name)}
        self.axis_plan = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "Saved", "%s_axis_plan.json" % key)

MODES = {
    "translationMode": "BMM_Ignore",
    "rotationMode": "BMM_Additive",
    "scaleMode": "BMM_Ignore",
    "rotationSpace": "BCS_BoneSpace",
}

# MakeRotator's three input pins, by the local axis each turns about. Roll is the bone's own
# X, Pitch its Y, Yaw its Z - which is why the axis plan reports a local axis and this maps
# it to a pin name rather than the other way round.
PIN_OF_COMPONENT = {"Roll": "Roll", "Pitch": "Pitch", "Yaw": "Yaw"}


def load_axis_plan(model):
    """{bone: (component, sign)}, measured by Tools/Python/build_aircraft_anim.py <model.key>.

    A MISSING FILE IS A CLEAR MESSAGE AND NOT A DEFAULT. Defaulting to Yaw here would wire a
    plausible graph off an assumption this script exists to stop making.

    FILLS model.plan AS A SIDE EFFECT, since Issue #294. The JSON row now carries the whole
    graph - bone, variable AND multiplier, not only the component and sign resolve_axis()
    measures - because Tools/wire_plane_anim.py no longer carries a per-aircraft PLAN of its
    own for this to join against; it reads the whole thing from here. CHAIN ORDER is the
    file's own row order, not re-sorted here - see driven_bone_plan()'s docstring for why the
    build step's order is already valid.
    """
    if not os.path.exists(model.axis_plan):
        sys.exit(
            "No %s.\n"
            "The rotation axes are MEASURED off the mesh's reference pose, not assumed.\n"
            "Close the editor and run:\n"
            "  UnrealEditor-Cmd.exe <project> -run=pythonscript "
            "-script=Tools/Python/build_aircraft_anim.py -unattended -nosplash -nopause %s\n"
            "then reopen it and run this again." % (model.axis_plan, model.key))
    with open(model.axis_plan) as handle:
        payload = json.load(handle)
    rows = payload["bones"]
    model.plan = [(row["bone"], row["variable"], row["multiplier"]) for row in rows]
    return {row["bone"]: (row["component"], row["sign"]) for row in rows}


# Sent to ProgrammaticToolset as ONE call. Doing it from here instead would be ~200 MCP round
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
    for row, (bone, var, mul, pin) in enumerate(PLAN):
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
        # THE PIN IS PER BONE, not Yaw for everything - see this file's header.
        axis = pin_index(mk_ni, "in", pin)
        if mul is None:
            connect(getter, 0, mk, axis)
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
            connect(m, pin_index(m_ni, "out", "ReturnValue"), mk, axis)

        connect(mk, pin_index(mk_ni, "out", "ReturnValue"), mb, pin_index(mb_ni, "in", "Rotation"))
        if prev is not None:
            connect(prev, 0, mb, pin_index(mb_ni, "in", "ComponentPose"))
        prev = mb

    # The ModifyBone chain is component space; the Output Pose wants local.
    c2l = node("Animation|ConvertSpaces|ComponentToLocal", -200, 0)
    connect(prev, 0, c2l, 0)
    connect(c2l, 0, root, 0)

    execute_tool(BT + "compile_blueprint", json.dumps({"blueprint": BP, "warnings_as_errors": False}))
    # THE SAVE'S ANSWER IS CARRIED BACK, because it can be NO. See main().
    saved = execute_tool(AT + "save_assets", json.dumps({"asset_paths": ["%(asset)s"]}))
    return {"wired": [b for b, _, _, _ in PLAN], "saved": saved}
'''


def saved_ok(answer):
    """Did save_assets actually say yes?

    LENIENT ABOUT THE SHAPE, STRICT ABOUT THE ANSWER. The tool has returned a bare bool here;
    a null would mean it answered nothing, which is not a yes. Anything that is not explicitly
    False or None is taken as success, so a future version returning a count or a list of
    paths does not start failing runs that worked.
    """
    return answer is not False and answer is not None


def connect_mcp(model):
    try:
        session, _ = Mcp.post({
            "jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                       "clientInfo": {"name": "wire_%s_anim" % model.key, "version": "1"}},
        })
    except OSError as exc:
        sys.exit("The editor is not running, or its MCP server is not up: %s" % exc)
    Mcp.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, session)
    return session


# THIRTEEN BONES IS ABOUT 200 TOOL CALLS IN ONE REQUEST, and it takes over a minute. Mcp.py's
# 30 s default is right for an interactive call and wrong for this one: the editor finishes
# the work regardless, so a client that gives up leaves a COMPLETED graph behind and prints a
# traceback that reads as failure. Measured 2026-09-21, wiring this graph for the first time.
WIRE_TIMEOUT = 600


def call(session, toolset, tool, args, timeout=Mcp.DEFAULT_TIMEOUT):
    result = Mcp.rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": {
        "toolset_name": toolset, "tool_name": tool, "arguments": args}}, timeout=timeout)
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


def read_chain(session, model):
    """Every driven bone, in chain order, walked back from Output Pose.

    ORDER IS READ, not assumed: it is one of the four decisions above, and a verifier that
    only checked membership would pass a graph that retracts the gear before it rolls the
    wheels - which on this rig spins each wheel inside its own bay.
    """
    nodes = call(session, BT, "find_nodes", {"graph": model.graph, "title": ""})
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


def expected(variable, mul, pin):
    """What read_chain() should report for one row.

    MakeRotator reads back with its three components IN ORDER - Roll, Pitch, Yaw - and only
    the driven one carries an expression, so the position of that expression IS the axis.
    That is what makes the axis verifiable at all rather than merely set.
    """
    driven = "Get%s()" % variable if mul is None else \
             "float*float(Get%s(),%f)" % (variable, mul)
    parts = ["0.0", "0.0", "0.0"]
    parts[["Roll", "Pitch", "Yaw"].index(pin)] = driven
    return "MakeRotator(%s)" % ",".join(parts)


def verify(session, model, axes):
    """Fails loudly, bone by bone. Returns True only if the graph matches PLAN exactly."""
    rows = read_chain(session, model)
    ok = True

    if len(rows) != len(model.plan):
        print("FAIL %d driven bones in the chain, the plan has %d"
              % (len(rows), len(model.plan)))
        ok = False

    for i, (bone, variable, mul) in enumerate(model.plan):
        if i >= len(rows):
            print("FAIL %-16s missing from the chain" % bone)
            ok = False
            continue
        pin = PIN_OF_COMPONENT[axes[bone][0]]
        got_bone, got_modes, got_rotation = rows[i]
        want = expected(variable, mul, pin)
        if got_bone != bone:
            print("FAIL position %d is %s, the plan says %s - the CHAIN ORDER is wrong"
                  % (i, got_bone, bone))
            ok = False
        if got_modes != MODES:
            print("FAIL %-16s modes %s" % (got_bone, got_modes))
            ok = False
        if got_rotation != want:
            print("FAIL %-16s rotation %s, wanted %s" % (got_bone, got_rotation, want))
            ok = False
        if got_bone == bone and got_modes == MODES and got_rotation == want:
            print("PASS %-16s %-5s %s" % (bone, pin, got_rotation))

    print("VERIFY %s" % ("PASS" if ok else "FAIL"))
    return ok


def main(argv, model):
    session = connect_mcp(model)

    if "--read" in argv:
        for bone, modes, rotation in read_chain(session, model):
            print("%-16s %s  %s" % (bone, rotation, modes))
        return 0

    axes = load_axis_plan(model)
    if "--verify" not in argv:
        # The axis column joins the plan here rather than in the Model itself, so that the
        # file on disk stays the only place it is stated.
        plan = [(bone, variable, mul, PIN_OF_COMPONENT[axes[bone][0]])
                for bone, variable, mul in model.plan]
        for bone, _, _, pin in plan:
            print("axis %-16s %s" % (bone, pin))
        # repr, NOT json.dumps. The result is pasted into a PYTHON source file, and
        # json.dumps writes None as `null`, which the sandbox rejects with "name 'null' is
        # not defined" - a runtime error from a script that parses perfectly.
        script = WIRE % {"bp": repr(model.bp), "graph": repr(model.graph),
                         "plan": repr(plan), "modes": repr(MODES),
                         "asset": model.bp_path}
        result = call(session, "editor_toolset.toolsets.programmatic.ProgrammaticToolset",
                      "execute_tool_script", {"script": script}, timeout=WIRE_TIMEOUT)
        wired = json.loads(result)
        print("wired: %s" % wired["wired"])
        # THE SAVE CAN RETURN FALSE AND WRITE NOTHING, AND THE VERIFY BELOW CANNOT SEE IT.
        #
        # This bit plane3 on 2026-09-21 and cost a whole round trip. The run wired thirteen
        # bones, compiled, printed VERIFY PASS for every one of them - and left the OLD
        # six-node graph on disk. Nothing was wrong with the graph; it existed only in the
        # editor's memory, and the editor was closed a minute later.
        #
        # THE VERIFY READS THE LIVE EDITOR, which is the right thing for checking that the
        # nodes and pins are what the plan asked for and the wrong thing for checking that
        # they survive. Those are two questions and only one of them was being asked. This is
        # the project's oldest trap - "both headless save APIs report success while writing
        # nothing" - in the one shape it had not yet taken here, where the API is honest and
        # says false and the caller throws the answer away.
        #
        # WHY IT SAID NO is not known and is deliberately not guessed at in this message: on
        # the run that failed, SK_Plane3_Skeleton was itself dirty and unsaved, which is the
        # obvious suspect and is not evidence. What the operator needs is to be told, and to
        # be told what to do, which costs nothing whether the guess is right or not.
        if not saved_ok(wired.get("saved")):
            print("FAIL the graph was wired and compiled but save_assets returned %r - it is "
                  "in the editor's memory and NOT on disk. Save All in the editor (Ctrl+Shift"
                  "+S) now, before closing it, then re-run this with --verify to confirm. Do "
                  "not trust the PASS lines above: they read the live editor, not the file."
                  % (wired.get("saved"),))
            return 1
    return 0 if verify(session, model, axes) else 1
