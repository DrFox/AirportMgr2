# Authoring an AnimGraph without the editor UI

**Verified 2026-09-20 against the running UE 5.8.2 editor.** Every claim below was measured,
not reasoned about; where the answer is "no", the engine source line that makes it no is
named. Four files in this repo asserted "UE 5.8 exposes no Python API for creating a node in
a Blueprint graph or connecting two pins". That is true of the `unreal` module those files
run in and FALSE of the engine, and it cost the project every AnimGraph being hand-wired.

## The verdict, in one table

| Step | Reachable? | How |
|---|---|---|
| Create the AnimBlueprint asset for a skeleton | YES | `unreal.AnimBlueprintFactory` + `AssetTools.create_asset`, headless. Already what `build_aircraft_anim.py plane1` does. |
| Duplicate an existing ABP as a template | YES | `AssetTools.duplicate` over MCP. Carries the SOURCE skeleton - fine for a variant, wrong for a new rig. |
| Add the float variables the graph is driven by | YES | `BlueprintTools.add_variable` |
| Create a Transform (Modify) Bone node | YES | `BlueprintTools.create_node` |
| Set its BoneToModify and the four modes | YES | `ObjectTools.set_properties` on the node's `node` struct |
| Connect pose pins and value pins | YES | `BlueprintTools.connect_pins` |
| Set a literal on an unconnected pin | YES | `set_pin_value` / `get_pin_value`, e.g. `"1.0,2.0,3.0"` for a Vector |
| Delete a node | YES | `BlueprintTools.delete_node` |
| Compile and save | YES | `BlueprintTools.compile_blueprint`, `AssetTools.save_assets` |
| Do all of the above in ONE round trip | YES | `ProgrammaticToolset.execute_tool_script` |
| Create the State Machine NODE in an AnimGraph | YES | `create_node "Animation|StateMachines|StateMachine"` |
| Put a state, conduit or transition INSIDE it | **NO** | two independent reasons, below |
| Read or write the graph as DSL | **NO** | `read_graph_dsl` returns `""` for any AnimGraph |

The whole of the reachable column needs the editor **RUNNING**, because it goes through
Epic's MCP server. The asset-creation row needs it **CLOSED**, because it is a commandlet.
**That is a sequencing cost of one editor restart, not a wall** - closing and reopening the
editor is allowed here, so the two halves are one unattended task:

```
Get-Process UnrealEditor* | Stop-Process -Force          # check for unsaved work first
UnrealEditor-Cmd.exe <project> -run=pythonscript -script=build_<model>_anim.py -unattended
Start-Process UnrealEditor.exe '"<project>"'             # ~20s; poll Tools/Mcp.py, don't sleep
<MCP wiring, compile, save>
```

Run end to end on 2026-09-20 and the result **diffed IDENTICAL** to hand-wired `ABP_Plane1`:
same five bones in the same chain order, same four modes on each, same
`MakeRotator(0,0, Angle * -1)` driver on each. Nothing about the graph says which one a human
made. The wiring script is idempotent - it deletes every node but Output Pose before it
starts - so a failed half-run costs nothing to clean up.

## What was actually run

**The whole pipeline, unattended.** The editor was killed, `ABP_PipelineProbe` was created by
a `-run=pythonscript` commandlet with `parent_class = UAirsideAgentAnim` and
`target_skeleton = SK_Plane1_Skeleton` (`MARKER: graphs=AnimGraph, EventGraph`, 23 KB on
disk), the editor was relaunched, and one `execute_tool_script` call wired all five of
plane1's bones - 5 getters, 4 multiplies, 5 MakeRotators, 5 ModifyBones with their modes, a
ComponentToLocal, and the chain into Output Pose - then compiled and saved it.

**It then diffed IDENTICAL against hand-wired `ABP_Plane1`**, by a reader that walks the pose
chain back from Output Pose and prints each node's bone, its four modes, and its rotation
source expression:

```
{"bone": "prop", "modes": ["BMM_Ignore","BMM_Additive","BMM_Ignore","BCS_BoneSpace"],
 "rotation": "MakeRotator(0.0,0.0,float*float(GetPropAngleDegrees(),-1.000000))"}
 ... nosewheel_steer last, exactly where ABP_Plane1 has it ...
IDENTICAL
```

Earlier, smaller probes established each primitive on its own: a `ABP_FactoryProbe` wired
from empty, a three-node chain built in one call, `set_pin_value`/`get_pin_value` round
tripped on a Vector pin, and `delete_node` removing a node that `find_nodes` then no longer
returned.

### One thing the diff caught

The first authored attempt came out DIFFERENT in exactly one respect: `nosewheel_steer` sat
before `nosewheel` in the chain, because that is what all three `build_*_anim.py` scripts
print - "WIRE nosewheel_steer BEFORE nosewheel". **`ABP_Plane1` and `ABP_Plane2` both ship
the opposite order**, steer LAST. The shipped order is the one the engine supports:
`FCSPose::SafeSetCSBoneTransforms` exists to "refresh any Children they have that has been
previously converted to Component Space", so steering the parent last carries the
already-rolled wheel with it. **The instruction in those three scripts disagrees with every
asset in the project and should be corrected** - it has not been, because it is a content
decision, not a documentation one.

That is the argument for authoring rather than clicking in one line: the click cannot be
diffed.

## The worked example in the repo

`Tools/wire_fueltruck_anim.py` is this whole document as a runnable script: it authors
`ABP_FuelTruck1`'s six bones, compiles, saves, and then reads the graph back and fails bone
by bone if anything disagrees. `--verify` runs the check alone and changes nothing. It lives
in `Tools/` and not `Tools/Python/` deliberately - everything in there is a commandlet that
needs the editor CLOSED, and this needs it UP. Copy it for the next model.

## The calls, in the form that works

Short tool names via `Tools/Mcp.py call`, fully qualified inside `execute_tool_script`.

```
Mcp.py call editor_toolset.toolsets.blueprint.BlueprintTools create_node \
  '{"graph":{"refPath":"/Game/X/ABP_X.ABP_X:AnimGraph"},
    "type_id":"Animation|SkeletalControls|Transform(Modify)Bone","pos":{"x":-600,"y":0}}'

Mcp.py call editor_toolset.toolsets.object.ObjectTools set_properties \
  '{"instance":{"refPath":"...:AnimGraph.AnimGraphNode_ModifyBone_0"},
    "values":"{\"node\":{\"boneToModify\":{\"boneName\":\"prop\"},
                \"rotationMode\":\"BMM_Additive\",\"rotationSpace\":\"BCS_BoneSpace\"}}"}'
```

Things that cost a call each to find out:

- **A variable's getter category is the C++ `UPROPERTY` Category**, so the inherited angles
  are `Variables|Airside|GetPropAngleDegrees`, not `Variables|Default|...`. Never compose a
  type_id; `find_node_types` it.
- **`Math|Float|float*float` is what a multiply READS BACK as and not what CREATES one.**
  Create `Utilities|Operators|Multiply` - a promotable operator, wildcard until something is
  plugged in. Connect `A` FIRST, then re-read the node: `B` and `ReturnValue` are float only
  afterwards, and their indices move.
- **`values` is a JSON STRING, not an object** - the properties live under a `node` key, the
  struct of the runtime `FAnimNode_*`. `ObjectTools.list_properties` on the node prints the
  whole schema including the enums' legal values.
- **A variable's getter type_id is `Variables|Default|GetX`, not `Variables|GetX`.** Ask
  `find_node_types` for it rather than composing it; the wrong one returns "does not exist".
- **Pin indices are positional, so look them up.** `get_node_infos` gives every pin's name
  and `index_id`; ModifyBone's are ComponentPose 0, Alpha 1, Translation 2, Rotation 3,
  Scale 4, but that is an observation, not a contract.
- **`find_nodes` requires `title`; `""` matches everything.**
- **A node's `type_id` reports the bone it was given** (`...Transform(Modify)Bone-Bone:prop`),
  which makes a wiring mistake visible without opening the asset.

## Why a state machine is not reachable

Two mechanisms, either of which alone would be fatal:

1. **`UBlueprintGraphEditor::ListAllNodes` is `TArray<UK2Node*>`**
   (`Engine/Source/Editor/BlueprintEditorLibrary/.../BlueprintGraphEditor.cpp:151`), and
   `UAnimStateNodeBase`, `UAnimStateTransitionNode` and `UAnimStateEntryNode` all derive
   from `UEdGraphNode`, not `UK2Node`. So the entire API is blind to them. `find_nodes` on a
   freshly created state machine graph returns `[]` although its `AnimStateEntryNode_0`
   exists and resolves by path - the schema creates one in `CreateDefaultNodesForGraph`.
   **An empty result from `find_nodes` is not evidence of an empty graph.**
2. **`CreateNodeFromName` spawns out of `FBlueprintActionDatabase`**, and "Add State" is not
   in it: it is a schema context action, `FEdGraphSchemaAction_NewStateNode`, built in
   `UAnimationStateMachineSchema::GetGraphContextActions`
   (`Engine/Source/Editor/AnimGraph/Private/AnimationStateMachineSchema.cpp:373`).

You will not even reach those. `create_node` and `find_node_types` on a state machine's inner
graph fail first with **`Blueprint: Cannot cast type 'AnimGraphNode_StateMachine' to
'Blueprint'`**, because the toolset's `_BlueprintCache` does
`unreal.Blueprint.cast(graph.get_outer())` and a state machine graph's outer is the node, not
the Blueprint. That is a toolset bug; fixing it would only move the failure to (1) and (2).

**So: a state machine is still hand work.** If a design needs one, the cheap shape is a state
machine authored once by hand in a base ABP and inherited, with the per-model bone work -
which is the part that varies and the part that is got wrong - done by MCP.

## Why the graph DSL is silent

`read_graph_dsl` returns `""` for an AnimGraph, and it did so for `ABP_Plane2`'s known-good
graph too, which is the control that stops the silence being read as an empty graph. The
decompiler starts from entry nodes and walks `Then` pins; an AnimGraph has no exec pins at
all, so it finds nothing to emit. The DSL is an exec-graph format. Use
`find_nodes` + `get_node_infos` + `ObjectTools.get_properties` to read an AnimGraph, and the
node calls above to write one.

## What this changes

Verifying a rig's wiring was already cheap. Producing it now is too, so the next model's
graph should be authored and READ BACK rather than clicked and eyeballed - plane1's graph
once passed a file-size check (24 KB -> 123 KB) while driving the nose WHEEL with
`SteerAngleDegrees` and leaving `nosewheel_steer` with no node at all.

The modes still matter and are still the thing that bites: Translation on Replace writes
(0,0,0) and detaches every driven part; Rotation on Replace discards the bind-pose
orientation. They are now settable AND readable, so assert them after writing.
