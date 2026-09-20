# Node Editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A deliberate Edit mode, entered on `M`, in which placed geometry can be dragged with the same snapping and guides as placement, and two nodes merge by dropping one on the other.

**Architecture:** `FBuildSession` gains `EGestureMode { Build, Edit }` and one `FEditTool`. `GetActiveTool()` returns the edit tool when the mode is Edit, so the build tool genuinely does not run and both drivers get the mode from one change. Which handles are grabbable comes from a new `EEditHandleKind` field **on `FToolRegistration`**, so the mapping cannot drift from the tool table. Snapping parity needs three things: the drag must use `GuidedCursor()`, the snap chain needs an exclusion so a dragged node stops claiming itself, and the drag needs its own guide anchor.

**Tech Stack:** UE 5.8.2, C++. `Airside` plugin (`Model/`, `Solve/`, `Tool/`, `Present/`), `AirportMgr` game module, `AirsideEditor` editor module, `AirsideTests` automation tests.

**Spec:** `docs/superpowers/specs/2026-09-20-node-editing-design.md` — read it before Task 1. It carries the rulings and the reasoning; this plan carries the steps.

## Global Constraints

- **Worktree:** all work happens in `C:\repos\airportmgr2-editing`, branch `feature/node-editing`, already created from `origin/main` at `c6dd84f`. Never build or edit `C:\repos\AirportMgr2` — the user's editor is open on it.
- **Build line** (worktree only — `-NoHotReloadFromIDE` is safe here and must NEVER be used on the main checkout):

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2-editing\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
```

- **Test line:**

```
./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-editing\AirportMgr.uproject
```

- **Never trust the runner's exit code.** Read the `N test(s) run, N failed, N crashed` line. A crashing test used to report green.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it. Build twice before believing a new test does not exist.
- **New classes with vtables, new `UCLASS`es and new `UPROPERTY`s need a full build**, not Live Coding. Tasks 1, 2, 3 and 6 all add types; batch their builds rather than expecting `Ctrl+Alt+F11` to cover them.
- **`Check-Architecture.ps1` runs first inside the test script** and fails the run before the editor starts. It enforces include direction, one log category per name, stacked doc comments.
- **Unity build:** test files share a translation unit. Prefix file-local test helpers uniquely (see `FLinkSink` in `GuidelineDrawToolTest.cpp`).
- **Test names must be distinct leaves.** UE's automation tree drops a bare-named parent once a dotted child exists, and only the run count catches it.
- **Comments explain WHY**, especially why an obvious alternative was rejected. Match the surrounding density; do not strip it.
- **Tools speak in ROAD PLANE coordinates and name a MEANING, never a colour.** `Tool/` must not include `Present/`.
- Log categories: `LogAirside` in the plugin, `LogRoadBuild` in the game module.

---

## File Structure

| File | Responsibility |
|---|---|
| `Plugins/Airside/Source/Airside/Public/Tool/EditTool.h` (create) | `FEditTool` — the one tool that runs while the mode is Edit |
| `Plugins/Airside/Source/Airside/Private/Tool/EditTool.cpp` (create) | its handles, drag, merge-on-drop and preview |
| `Plugins/Airside/Source/Airside/Public/Tool/RoadGuideAnchor.h` (create) | `AddNodeCandidates` — the candidate loop both anchors share |
| `Plugins/Airside/Source/Airside/Private/Tool/RoadGuideAnchor.cpp` (create) | its body, lifted verbatim from `FRoadDrawTool` |
| `Tool/BuildSession.h/.cpp` (modify) | `EGestureMode`, `EEditHandleKind` on `FToolRegistration`, mode-aware `GetActiveTool` |
| `Tool/RoadBuildTool.h` (modify) | `FToolContext::EditHandles`, `IBuildTool::GetSnapExclusion`, `EPreviewStyle::Handle` |
| `Tool/RoadSnap.h/.cpp` (modify) | `FRoadSnapQuery` with `ExcludeNode` |
| `Tool/RoadDrawTool.h/.cpp` (modify) | drag removed; candidate loop delegated |
| `Model/RoadNetwork.h/.cpp` (modify) | `MergeNodes` |
| `Present/RoadEditFacade.h/.cpp`, `Present/RoadNetworkActor.h/.cpp`, `Tool/RoadEditTarget.h` (modify) | `MergeNodes`, `MoveApronCorner` on the facade seam |
| `Source/AirportMgr/BuildActions.cpp` (modify) | the `edit.editmode` row — key, bar button and banner from one list |
| `Plugins/Airside/Source/AirsideTests/Private/EditToolTest.cpp` (create) | mode, handles, snapping, drag |
| `Plugins/Airside/Source/AirsideTests/Private/MergeNodesTest.cpp` (create) | the model-level merge rules |

---

### Task 1: The mode, and a tool that suppresses building

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/EditTool.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/EditTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp:91-94` (`GetActiveTool`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/EditToolTest.cpp` (create)

**Interfaces:**
- Produces: `enum class EGestureMode : uint8 { Build, Edit };`, `FBuildSession::SetGestureMode(EGestureMode)`, `FBuildSession::GetGestureMode() const`, `class FEditTool : public IBuildTool`.
- Consumes: `IBuildTool`, `FToolContext`, `FBuildSession` as they stand.

- [ ] **Step 1: Write the failing test**

Create `EditToolTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/EditTool.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEditModeSuppressesTheBuildToolTest,
    "Airside.Tool.EditModeSuppressesTheBuildTool",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeSuppressesTheBuildToolTest::RunTest(const FString& Parameters)
{
    FAirsideTestWorld TestWorld;
    if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
    ARoadNetworkActor* Actor = TestWorld.Actor;
    if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

    FBuildSession Session;
    TestTrue(TEXT("a session opens in Build, so nothing is editable until asked"),
        Session.GetGestureMode() == EGestureMode::Build);

    // Taxiway, so the tool under test is one that DOES build on a click.
    Session.SelectTool(1);
    Session.SetGestureMode(EGestureMode::Edit);

    FBuildSessionTunables Tunables;
    const FToolContext Context =
        Session.MakeContext(Actor, FVector2D(1000.0, 1000.0), Tunables, false, false);

    IBuildTool* Active = Session.GetActiveTool();
    if (!TestNotNull(TEXT("a tool is active in Edit"), Active)) { return false; }
    Active->OnClick(Context);

    // THE WHOLE POINT OF THE MODE. A click that built here is the misclick this feature
    // exists to remove, only in the opposite direction.
    const URoadNetwork* Network = Actor->GetNetwork();
    const int32 Nodes = Network != nullptr ? Network->GetNodes().Num() : 0;
    TestEqual(TEXT("a click in Edit builds nothing, because the build tool does not run"),
        Nodes, 0);

    Session.SetGestureMode(EGestureMode::Build);
    Session.GetActiveTool()->OnClick(Context);
    TestEqual(TEXT("and the same click in Build does build, so the test is measuring the mode "
                   "and not a broken tool"),
        Actor->GetNetwork()->GetNodes().Num(), 1);
    return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Build twice (new test file), then:
`./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-editing\AirportMgr.uproject -Filter Airside.Tool.EditMode`
Expected: compile error — `EGestureMode` and `Tool/EditTool.h` do not exist.

- [ ] **Step 3: Create `FEditTool`**

`Public/Tool/EditTool.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * The one tool that runs while the session's mode is Edit - Strategy, like every other
 * IBuildTool, and picked the same way: by the session, not by a transition.
 *
 * IT BUILDS NOTHING, and that is the feature rather than an omission. Editing placed
 * geometry was ruled to require a deliberate act (design doc section 3), and the symmetry
 * that settled it cuts both ways: you must not be able to build by accident while
 * reaching for a node, any more than you may move a node while reaching to build. So the
 * mode SUPPRESSES the build tool through FBuildSession::GetActiveTool rather than
 * re-purposing its drags - nine tools each remembering to refuse is nine places that must
 * agree, and eight of them have no edit to offer at all.
 *
 * WHICH handles are grabbable is not this tool's choice. It reads
 * FToolContext::EditHandles, which MakeContext fills from the lit tool's registry entry -
 * so the Taxiway button lights taxiway nodes and the Apron button lights apron corners,
 * and the mapping lives in the one table rather than in a switch here that must agree
 * with it.
 */
class AIRSIDE_API FEditTool : public IBuildTool
{
public:
    virtual FText GetDisplayName() const override;

    /** Nothing. Edit does not build - see the class comment. */
    virtual void OnClick(const FToolContext& Context) override {}
    virtual void OnCancel(const FToolContext& Context) override {}
    virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

    /** Always, for now: nothing is part-drawn because nothing is drawn. A drag is a
     *  gesture, not a drawing step - the same distinction FRoadDrawTool's header draws
     *  about its own drag. Task 4 revisits this once a drag exists to be idle about. */
    virtual bool IsIdle() const override { return true; }
};
```

`Private/Tool/EditTool.cpp`:

```cpp
#include "Tool/EditTool.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FEditTool::GetDisplayName() const
{
    return LOCTEXT("EditTool", "Edit");
}

void FEditTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
    // Handles arrive in Task 4. Empty rather than absent so the seam exists and the
    // overlay has something to call from the first build.
}

#undef LOCTEXT_NAMESPACE
```

- [ ] **Step 4: Add the mode to `FBuildSession`**

In `BuildSession.h`, above `class FBuildSession`:

```cpp
/**
 * Build or Edit - what a gesture MEANS, across every tool.
 *
 * AN ENUM AND NOT A bool (CLAUDE.md: a phase is an enum, never a set of bools). Two
 * values today; it leaves room for the third this design deliberately did not build -
 * Upgrade, repainting an existing road to the current width - without a second flag that
 * could be true at the same time as this one.
 *
 * ON THE SESSION, not on either driver, so PIE and URoadBuildEdMode cannot disagree about
 * it. FRoadSnapSettings' own header records what the two drivers holding private copies of
 * a shared decision cost the last time.
 */
enum class EGestureMode : uint8
{
    Build,
    Edit
};
```

Add to the public section of `FBuildSession`:

```cpp
    /** Build or Edit. Resets to Build on construction: a mode that survived into a new
     *  session would be a drag the player never asked to be able to make. */
    EGestureMode GetGestureMode() const { return Mode; }

    /**
     * Switch between building and editing, deactivating the outgoing tool so nothing is
     * left part-drawn to reappear - the same argument SelectTool makes for the same call.
     */
    void SetGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext = FToolContext());
```

and to the private section, beside `ActiveTool`:

```cpp
    EGestureMode Mode = EGestureMode::Build;

    /**
     * The tool that runs while Mode is Edit. A MEMBER rather than an entry in Tools,
     * because it is not selected by a number key and must not appear on the tool row -
     * it is the other axis. mutable for the same reason Selection is: GetActiveTool is
     * const and hands out a pointer the driver drives.
     */
    mutable FEditTool EditTool;
```

- [ ] **Step 5: Make `GetActiveTool` mode-aware**

In `BuildSession.cpp`, replace `GetActiveTool`:

```cpp
IBuildTool* FBuildSession::GetActiveTool() const
{
    // THE ONE PLACE THE MODE IS HONOURED. Both drivers reach every tool through this
    // function (nine call sites in ARoadBuildController, eight in URoadBuildEditorTool),
    // so returning the edit tool here is what makes "Edit suppresses the build tool"
    // structural rather than a rule eight tools have to remember.
    if (Mode == EGestureMode::Edit)
    {
        return &EditTool;
    }
    return Tools.IsValidIndex(ActiveTool) ? Tools[ActiveTool].Get() : nullptr;
}

void FBuildSession::SetGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext)
{
    if (InMode == Mode)
    {
        return;
    }

    // The outgoing tool abandons whatever it had part-drawn, exactly as SelectTool does.
    // Switching to Edit mid-chain and back would otherwise resume a road the player
    // stopped drawing to go and fix something else.
    if (IBuildTool* Outgoing = GetActiveTool())
    {
        Outgoing->OnDeactivate(DeactivateContext);
    }

    Mode = InMode;
    UE_LOG(LogAirside, Log, TEXT("Gesture mode -> %s"),
        Mode == EGestureMode::Edit ? TEXT("Edit") : TEXT("Build"));
}
```

Add `#include "Tool/EditTool.h"` to `BuildSession.h` and confirm `AirsideLog.h` is included in `BuildSession.cpp`.

- [ ] **Step 6: Build twice, run the test**

Build, build again, then run the filter. Expected: `Airside.Tool.EditModeSuppressesTheBuildTool` passes. Read the `N run, N failed, N crashed` line.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/EditTool.h \
        Plugins/Airside/Source/Airside/Private/Tool/EditTool.cpp \
        Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h \
        Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp \
        Plugins/Airside/Source/AirsideTests/Private/EditToolTest.cpp
git commit -m "feat(airside): an Edit gesture mode that suppresses the build tool

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Which handles are live — one list

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h` (`FToolRegistration`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp:27-80` (the registry), `MakeContext`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (`FToolContext`)
- Test: `EditToolTest.cpp` (append)

**Interfaces:**
- Consumes: `EGestureMode`, `FEditTool` from Task 1.
- Produces: `enum class EEditHandleKind : uint8 { None, AirsideNode, ServiceRoadNode, RunwayThreshold, ApronCorner };`, `FToolRegistration::EditHandles`, `FToolContext::EditHandles`.

- [ ] **Step 1: Write the failing test**

Append to `EditToolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEditHandlesAreDeclaredForEveryRegistryEntryTest,
    "Airside.Tool.EditHandlesAreDeclaredForEveryRegistryEntry",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesAreDeclaredForEveryRegistryEntryTest::RunTest(const FString& Parameters)
{
    // NAMES, NOT COUNTS. CLAUDE.md: where UE forces two lists the consumer checks identity
    // and logs both on mismatch. A count would pass on a table where two entries had
    // swapped their handle kinds.
    const TMap<FName, EEditHandleKind> Expected = {
        { TEXT("Select"),          EEditHandleKind::None            },
        { TEXT("Taxiway"),         EEditHandleKind::AirsideNode     },
        { TEXT("Apron"),           EEditHandleKind::ApronCorner     },
        { TEXT("Stand"),           EEditHandleKind::None            },
        { TEXT("Guideline"),       EEditHandleKind::None            },
        { TEXT("Runway"),          EEditHandleKind::RunwayThreshold },
        { TEXT("HoldingPosition"), EEditHandleKind::None            },
        { TEXT("Road"),            EEditHandleKind::ServiceRoadNode },
        { TEXT("FuelDepot"),       EEditHandleKind::None            },
    };

    for (const FToolRegistration& Entry : ToolRegistry())
    {
        const EEditHandleKind* Want = Expected.Find(Entry.Id);
        if (!TestNotNull(*FString::Printf(
                TEXT("registry entry '%s' is named in this test - a new tool must declare "
                     "what Edit means for it, even if the answer is None"),
                *Entry.Id.ToString()), (void*)Want))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("'%s' declares the expected edit handles"),
            *Entry.Id.ToString()), Entry.EditHandles == *Want);
    }
    TestEqual(TEXT("and the table has no entry this test has not heard of"),
        ToolRegistry().Num(), Expected.Num());
    return true;
}
```

Check the ninth entry's real `Id` in `BuildSession.cpp` before running — the depot's may be `FuelDepot` or `Depot`. Use whatever the table says.

- [ ] **Step 2: Run to verify it fails**

Expected: compile error — `EEditHandleKind` and `FToolRegistration::EditHandles` do not exist.

- [ ] **Step 3: Add the enum and the field**

`EEditHandleKind` goes in **`RoadBuildTool.h`**, above `FToolContext`, and NOT in
`BuildSession.h`. Both headers need it, `BuildSession.h` already includes
`RoadBuildTool.h`, and adding the reverse include would close a cycle that
`Check-Architecture.ps1` fails. Put this above `struct FToolContext`:

```cpp
/**
 * What a tool exposes for editing while the session's mode is Edit.
 *
 * ON FToolRegistration rather than in a switch beside it, because this is a list that
 * must agree with the tool table and CLAUDE.md's answer to that is to make it ONE list.
 * The alternative - a map from tool id to handle kind, living in FEditTool - is exactly
 * the shape this codebase has shipped three times and regretted: a list nothing checks
 * against the one it mirrors.
 *
 * A NODE CARRIES NO KIND (see FRoadNode); kind lives on the segment's Profile. So
 * AirsideNode and ServiceRoadNode are questions about a node's ARMS, answered through
 * RoadNaming::ReferenceOf - the one home for that classification. A node where a service
 * road meets a taxiway is grabbable under both, correctly, because it is both.
 */
enum class EEditHandleKind : uint8
{
    /** This tool has nothing to edit. The bar greys the Edit toggle and says so. */
    None,

    /** A node with an incident taxiway-or-runway segment. */
    AirsideNode,

    /** A node with an incident service-road segment. */
    ServiceRoadNode,

    /** The END node of a runway chain - not its interior nodes, which a split may have made. */
    RunwayThreshold,

    /** A corner of an apron outline. */
    ApronCorner,
};
```

Add to `FToolRegistration`, after `Tooltip`:

```cpp
    /** What this tool exposes while the mode is Edit. None greys the toggle out. */
    EEditHandleKind EditHandles = EEditHandleKind::None;
```

- [ ] **Step 4: Fill the registry**

In `BuildSession.cpp`, the entries are aggregate-initialised. Add the sixth member to each, in order. Do NOT rely on the default for the four that edit something — spell every one of the nine out, so a reader sees the whole column:

```cpp
        { EKeys::Four,  TEXT("Select"),   LOCTEXT("Select",    "Select"),
            LOCTEXT("SelectTooltip", "..."),
            [] { return MakeUnique<FSelectTool>(); }, EEditHandleKind::None },

        { EKeys::One,   TEXT("Taxiway"),  LOCTEXT("Taxiway",   "Taxiway"),
            LOCTEXT("TaxiwayTooltip", "..."),
            [] { return MakeUnique<FRoadDrawTool>(ERoadKind::Taxiway); },
            EEditHandleKind::AirsideNode },
```

and so on for Apron (`ApronCorner`), Stand (`None`), Guideline (`None`), Runway (`RunwayThreshold`), HoldingPosition (`None`), Road (`ServiceRoadNode`), the depot (`None`). Keep each existing tooltip string exactly as it is — Task 4 changes the Taxiway one and nothing else does.

- [ ] **Step 5: Carry it on the context**

In `RoadBuildTool.h`, add to `FToolContext` beside `SnapRadius`:

```cpp
    /**
     * What the LIT tool exposes for editing, from its registry entry. Meaningful only
     * while FBuildSession's mode is Edit; None at every other moment.
     *
     * ON THE CONTEXT rather than on FEditTool, for the reason this struct's own header
     * gives about holding no state: a context is built fresh each frame, so the edit tool
     * cannot keep a stale view of which tool is lit. Switching from Taxiway to Apron with
     * Edit held would otherwise leave road nodes grabbable until something thought to
     * push the change.
     */
    EEditHandleKind EditHandles = EEditHandleKind::None;
```

`BuildSession.h` sees `EEditHandleKind` through the include it already has; add no new include in either direction.

In `BuildSession.cpp`'s `MakeContext`, after `Context.SnapRadius = ...`:

```cpp
    // FROM THE LIT TOOL'S REGISTRY ENTRY, not from the edit tool's own idea of what is
    // grabbable: the registry is the one list, and this is where it is READ.
    const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
    Context.EditHandles = (Mode == EGestureMode::Edit && Registry.IsValidIndex(ActiveTool))
        ? Registry[ActiveTool].EditHandles
        : EEditHandleKind::None;
```

- [ ] **Step 6: Build and run**

Expected: both `Airside.Tool.*` tests pass.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(airside): every tool declares what Edit means for it

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: The snap chain learns to exclude

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadSnap.h:120-195`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadSnap.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (`IBuildTool`)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`, `Private/Tool/BuildSession.cpp` (`ResolveSnap`, `MakeContext`)
- Test: `EditToolTest.cpp` (append)

**Interfaces:**
- Produces: `struct FRoadSnapQuery { FVector2D Cursor; FRoadNodeId ExcludeNode; };`, `IBuildTool::GetSnapExclusion() const`, `FRoadSnapChain::Resolve(const URoadNetwork&, const FRoadSnapQuery&, const FRoadSnapSettings&) const`.
- The three-argument `Resolve(Network, Cursor, Settings)` **stays**, as a forwarder.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSnapChainExcludesTheDraggedNodeTest,
    "Airside.Tool.SnapChainExcludesTheDraggedNode",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapChainExcludesTheDraggedNodeTest::RunTest(const FString& Parameters)
{
    FAirsideTestWorld TestWorld;
    ARoadNetworkActor* Actor = TestWorld.Actor;
    if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

    const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
    const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
    Actor->ConnectNodes(A, B);

    const URoadNetwork* Network = Actor->GetNetwork();
    FRoadSnapChain Chain;
    FRoadSnapSettings Settings;

    // WITHOUT the exclusion, a cursor sitting on A resolves to A. That is correct for a
    // click and fatal for a drag: the node being dragged claims its own cursor and can
    // never move anywhere.
    {
        const FRoadSnapResult Hit = Chain.Resolve(*Network, FVector2D(0.0, 0.0), Settings);
        TestTrue(TEXT("a cursor on a node resolves to that node, as a click needs"),
            Hit.Kind == ERoadSnapKind::Node && Hit.Node.Index == A);
    }

    // WITH it, the same cursor falls through. A is excluded, and B is 60 m away - well
    // outside NodeRadius - so nothing claims it.
    {
        FRoadSnapQuery Query;
        Query.Cursor = FVector2D(0.0, 0.0);
        Query.ExcludeNode = Network->NodeIdAt(A);

        const FRoadSnapResult Hit = Chain.Resolve(*Network, Query, Settings);
        TestTrue(TEXT("the excluded node does not claim its own cursor, which is what lets "
                      "a dragged node move at all"),
            Hit.Kind != ERoadSnapKind::Node);
    }

    // AND THE EXCLUSION IS NOT A BLANKET REFUSAL: a cursor on B still finds B while A is
    // excluded, which is the merge target case and the reason the field is a node and not
    // a bool.
    {
        FRoadSnapQuery Query;
        Query.Cursor = FVector2D(6000.0, 0.0);
        Query.ExcludeNode = Network->NodeIdAt(A);

        const FRoadSnapResult Hit = Chain.Resolve(*Network, Query, Settings);
        TestTrue(TEXT("another node is still found while one is excluded"),
            Hit.Kind == ERoadSnapKind::Node && Hit.Node.Index == B);
    }
    return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error — `FRoadSnapQuery` does not exist.

- [ ] **Step 3: Add the query struct**

In `RoadSnap.h`, above `struct IRoadSnapRule`:

```cpp
/**
 * Where the cursor is, and what it may NOT claim.
 *
 * A STRUCT AND NOT A FOURTH PARAMETER, per CLAUDE.md's "one struct per thing": the cursor
 * and its exclusion are one question, and the next exclusion to arrive adds a field here
 * rather than a parameter to every rule, the chain, ResolveSnap and MakeContext.
 *
 * THE EXCLUSION EXISTS FOR THE DRAG. Dragging node A puts the cursor ON A, so
 * FRoadNodeSnapRule claims A, and the node is pinned to its own position for ever - the
 * whole reason a drag could not snap before this. It is a NODE and not a bool because the
 * drag must still find OTHER nodes: that is how a merge target is picked.
 */
struct AIRSIDE_API FRoadSnapQuery
{
    FVector2D Cursor = FVector2D::ZeroVector;

    /** Unset for every caller that is not dragging - which is all of them but one. */
    FRoadNodeId ExcludeNode;
};
```

- [ ] **Step 4: Thread it through the rules and the chain**

Change `IRoadSnapRule::Resolve`, `FRoadNodeSnapRule::Resolve` and `FRoadSegmentSnapRule::Resolve` to take `const FRoadSnapQuery& Query` in place of `const FVector2D& Cursor`. In `FRoadSnapChain`, keep both:

```cpp
    /** First rule to claim the query wins; Free when none does. */
    FRoadSnapResult Resolve(const URoadNetwork& Network, const FRoadSnapQuery& Query,
        const FRoadSnapSettings& Settings) const;

    /**
     * No exclusion - what every caller before the drag existed meant, and what a CLICK
     * always means. Kept so no existing call site changes (CLAUDE.md: every reachable
     * entry point stays reachable at its old name, as a forwarder if the logic moved).
     */
    FRoadSnapResult Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
        const FRoadSnapSettings& Settings) const
    {
        FRoadSnapQuery Query;
        Query.Cursor = Cursor;
        return Resolve(Network, Query, Settings);
    }
```

In `RoadSnap.cpp`, inside `FRoadNodeSnapRule::Resolve`'s loop over nodes, add beside the existing `bAlive` check:

```cpp
        // THE NODE BEING DRAGGED DOES NOT CLAIM ITS OWN CURSOR. Compared by handle rather
        // than by position: two nodes may legitimately sit at the same coordinates for the
        // frame before a merge resolves them, and a position test would silently exclude
        // the merge TARGET as well as the node being dragged.
        if (Query.ExcludeNode.IsSet() && Query.ExcludeNode.Index == Index)
        {
            continue;
        }
```

`FRoadSegmentSnapRule` needs the matching guard: a segment incident to the dragged node must not be splittable by that node's own cursor. Skip any segment with `ExcludeNode` as an endpoint.

- [ ] **Step 5: Let a tool declare its exclusion**

In `RoadBuildTool.h`, on `IBuildTool`:

```cpp
    /**
     * A node this tool's gesture is MOVING, which must not be snapped to.
     *
     * Silent by default: only a drag has one. Read by FBuildSession::MakeContext while it
     * builds the context, which is why this takes no context - there is none yet. The same
     * argument DescribeGuideAnchor's own comment makes about being handed the target
     * instead.
     *
     * A SLOT INDEX, like every other index a tool passes across the IRoadEditTarget seam:
     * the facade makes the generation-checked handles, in the one place that refuses a
     * dead slot.
     */
    virtual int32 GetSnapExclusion() const { return INDEX_NONE; }
```

- [ ] **Step 6: Use it in `MakeContext`**

Change `FBuildSession::ResolveSnap` to take the query, keeping its old signature as a forwarder for the two drivers that call it directly (`RoadBuildController.cpp:418` is one — check for others with `grep -rn "ResolveSnap"`). In `MakeContext`, replace the `ResolveSnap` call:

```cpp
    FRoadSnapQuery Query;
    Query.Cursor = PlaneHit;

    // WHAT THE ACTIVE TOOL IS MOVING, so a drag stops snapping to the node in its own hand.
    // Asked here and not inside the chain because only the tool knows, and only this
    // function has both the tool and the query.
    const IBuildTool* Snapping = GetActiveTool();
    if (Snapping != nullptr && Network != nullptr)
    {
        const int32 Exclude = Snapping->GetSnapExclusion();
        if (Exclude != INDEX_NONE)
        {
            Query.ExcludeNode = Network->NodeIdAt(Exclude);
        }
    }

    FRoadSnapResult Snapped;
    ResolveSnap(Network, Query, Tunables.Snap, Snapped);
```

- [ ] **Step 7: Build and run the full suite**

Run the WHOLE suite, not the filter: this changed a signature three rules and both drivers use. Expected: `0 failed, 0 crashed`, and the run count no lower than before this task.

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "feat(airside): the snap chain can exclude the node being dragged

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Handles, and a drag that snaps

**Files:**
- Modify: `Tool/EditTool.h/.cpp`
- Modify: `Tool/RoadBuildTool.h` (`EPreviewStyle`)
- Modify: `Tool/RoadDrawTool.h:110-118,152-155` and `RoadDrawTool.cpp:541-580,595` — the drag comes out
- Modify: `Private/Tool/BuildSession.cpp` — the Taxiway tooltip
- Test: `EditToolTest.cpp` (append)

**Interfaces:**
- Consumes: `FToolContext::EditHandles` (Task 2), `IBuildTool::GetSnapExclusion` (Task 3).
- Produces: `EPreviewStyle::Handle`, `FEditTool::OnDragBegin/OnDrag/OnDragEnd`, `FEditTool::GetDragNode() const`.

- [ ] **Step 1: Write the two failing tests**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEditModeDragSnapsExactlyToANodeTest,
    "Airside.Tool.EditModeDragSnapsExactlyToANode",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDragSnapsExactlyToANodeTest::RunTest(const FString& Parameters)
{
    FAirsideTestWorld TestWorld;
    ARoadNetworkActor* Actor = TestWorld.Actor;
    if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

    // Three nodes: a road A-B, and a lone C for A to be dragged onto. C is far enough from
    // B that nothing but C can claim the drop.
    const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
    const int32 B = Actor->PlaceNode(FVector2D(8000.0, 0.0));
    Actor->ConnectNodes(A, B);
    const int32 C = Actor->PlaceNode(FVector2D(0.0, 9000.0));

    const FVector2D CPosition = Actor->GetNetwork()->GetNodes()[C].Position;

    FBuildSession Session;
    Session.SelectTool(1);                       // Taxiway: lights AirsideNode handles
    Session.SetGestureMode(EGestureMode::Edit);
    FBuildSessionTunables Tunables;

    // Grab A.
    Session.GetActiveTool()->OnDragBegin(
        Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));

    // Drag to just SHORT of C - inside its snap radius but not on it. A click here would
    // land on C exactly; the drag must do the same.
    const FVector2D NearC = CPosition - FVector2D(0.0, 80.0);
    Session.GetActiveTool()->OnDrag(Session.MakeContext(Actor, NearC, Tunables, false, false));

    const FVector2D Landed = Actor->GetNetwork()->GetNodes()[A].Position;

    // BITWISE, not within a tolerance. FRoadSnapResult's own contract is that a Node snap
    // carries the node's stored position copied verbatim - "not the cursor, and not a
    // recomputed value". A near-miss here is a drag that merely looks snapped.
    TestTrue(TEXT("the dragged node lands on exactly the coordinates the graph holds for "
                  "the node it snapped to, as a click would"),
        Landed.X == CPosition.X && Landed.Y == CPosition.Y);

    Session.GetActiveTool()->OnDragEnd(
        Session.MakeContext(Actor, NearC, Tunables, false, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoadDrawToolNoLongerDragsNodesTest,
    "Airside.Tool.RoadDrawToolNoLongerDragsNodes",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawToolNoLongerDragsNodesTest::RunTest(const FString& Parameters)
{
    FAirsideTestWorld TestWorld;
    ARoadNetworkActor* Actor = TestWorld.Actor;
    if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

    const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
    const int32 B = Actor->PlaceNode(FVector2D(8000.0, 0.0));
    Actor->ConnectNodes(A, B);
    const FVector2D Was = Actor->GetNetwork()->GetNodes()[A].Position;

    FBuildSession Session;
    Session.SelectTool(1);                       // Taxiway, in BUILD mode
    FBuildSessionTunables Tunables;

    IBuildTool* Tool = Session.GetActiveTool();
    Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));
    Tool->OnDrag(Session.MakeContext(Actor, FVector2D(500.0, 500.0), Tunables, false, false));
    Tool->OnDragEnd(Session.MakeContext(Actor, FVector2D(500.0, 500.0), Tunables, false, false));

    // THE MISCLICK, PINNED. A press that travelled over a node used to reshape the road
    // with no way to decline. Editing is a deliberate act now (design doc section 3), so a
    // drag in Build mode must do nothing at all.
    const FVector2D Now = Actor->GetNetwork()->GetNodes()[A].Position;
    TestTrue(TEXT("a drag under the road tool moves nothing - editing needs the Edit mode"),
        Now.X == Was.X && Now.Y == Was.Y);
    return true;
}
```

- [ ] **Step 2: Run to verify both fail**

Expected: the first fails (no drag on `FEditTool`, so A does not move), the second fails (the road tool still drags).

- [ ] **Step 3: Add the preview style**

In `RoadBuildTool.h`, at the **very end** of `EPreviewStyle`, after `Guide`:

```cpp
    /**
     * A point the Edit mode can grab. Drawn for every handle the lit tool exposes, so
     * "what can I move" is answered by looking rather than by trying.
     *
     * NOT Hover, which already means "what a click would select right now" and is drawn
     * for ONE thing - the pickable under the cursor. A handle is drawn for all of them at
     * once, and the two are on screen in the same frame.
     *
     * AT THE END, like Pinned, Provisional and Guide above and for the same reason: this
     * is a UENUM and renumbering it repoints any value already serialised against it.
     */
    Handle,
```

- [ ] **Step 4: Give `FEditTool` its handles and its drag**

Add to `EditTool.h`:

```cpp
    virtual void OnDragBegin(const FToolContext& Context) override;
    virtual void OnDrag(const FToolContext& Context) override;
    virtual void OnDragEnd(const FToolContext& Context) override;
    virtual void OnDeactivate(const FToolContext& Context) override;

    /** Nothing is part-drawn unless a drag is live. A drag is a gesture rather than a
     *  drawing step - FRoadDrawTool's header draws the same distinction about its own. */
    virtual bool IsIdle() const override { return DragNode == INDEX_NONE; }

    /** The node this tool is moving, so MakeContext can keep the snap chain off it. */
    virtual int32 GetSnapExclusion() const override { return DragNode; }

    /** For tests and the overlay. */
    int32 GetDragNode() const { return DragNode; }

    /**
     * Every node slot the lit tool exposes, in index order. Static so the preview and the
     * drag cannot disagree about what is grabbable, and so a test can ask directly.
     *
     * Apron corners are NOT nodes and are not returned here - Task 10 gives them their own
     * accessor rather than pretending a corner is a node index.
     */
    static void GatherNodeHandles(const FToolContext& Context, TArray<int32>& Out);

private:
    /** The node held by a live drag, or INDEX_NONE. */
    int32 DragNode = INDEX_NONE;
```

In `EditTool.cpp`:

```cpp
void FEditTool::GatherNodeHandles(const FToolContext& Context, TArray<int32>& Out)
{
    Out.Reset();
    const URoadNetwork* Network = Context.Network();
    if (Network == nullptr)
    {
        return;
    }

    const TArray<FRoadNode>& Nodes = Network->GetNodes();
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        if (!Nodes[Index].bAlive)
        {
            // A DELETED NODE KEEPS ITS SLOT. Offering one would draw a handle on a
            // junction the player has removed - the same guard the guide candidates make.
            continue;
        }

        bool bWanted = false;
        for (const FRoadSegmentId Arm : Nodes[Index].Incident)
        {
            SnapGuide::EReference Of = SnapGuide::EReference::Taxiway;
            if (!RoadNaming::ReferenceOf(*Network, Arm, Of))
            {
                continue;
            }
            // THROUGH RoadNaming, the ONE classification (see its header). Asking the
            // profile's guidelines here would be the fourth private copy of a question
            // that header records being answered four ways once already.
            switch (Context.EditHandles)
            {
            case EEditHandleKind::AirsideNode:
                bWanted |= (Of == SnapGuide::EReference::Taxiway
                         || Of == SnapGuide::EReference::Runway);
                break;
            case EEditHandleKind::ServiceRoadNode:
                bWanted |= (Of == SnapGuide::EReference::ServiceRoad);
                break;
            case EEditHandleKind::RunwayThreshold:
                // A THRESHOLD IS AN END, not any runway node: a split runway has interior
                // nodes, and dragging one of those sideways would kink the strip rather
                // than reposition it. One incident runway arm is what makes it an end.
                bWanted |= (Of == SnapGuide::EReference::Runway
                         && Nodes[Index].Incident.Num() == 1);
                break;
            default:
                break;
            }
        }

        if (bWanted)
        {
            Out.Add(Index);
        }
    }
}

void FEditTool::OnDragBegin(const FToolContext& Context)
{
    if (Context.Target == nullptr || Context.Snap.Kind != ERoadSnapKind::Node)
    {
        return;
    }

    // ONLY A HANDLE THIS TOOL OFFERED. The snap chain finds every node; the lit tool
    // decides which ones this mode may touch, and a drag that ignored the filter would
    // move an apron corner's node while the Apron button was lit for a different reason.
    TArray<int32> Handles;
    GatherNodeHandles(Context, Handles);
    if (!Handles.Contains(Context.Snap.Node.Index))
    {
        return;
    }

    DragNode = Context.Snap.Node.Index;

    // One undo step for the whole drag, not one per frame.
    Context.Target->BeginInteractiveEdit(TEXT("move node"));
    UE_LOG(LogAirside, Log, TEXT("Edit: grabbed node %d"), DragNode);
}

void FEditTool::OnDrag(const FToolContext& Context)
{
    if (DragNode == INDEX_NONE || Context.Target == nullptr)
    {
        return;
    }

    // THE SNAPPED, GUIDED POSITION - the whole of "editing should feel like placing".
    // FRoadDrawTool passed the RAW cursor here, which is why a drag ignored both the snap
    // chain and the guides that landed in #162. Snap first, because a Node snap carries the
    // target's stored coordinates verbatim and that exactness is what a merge needs; the
    // guide otherwise, which is what a free drag follows.
    const FVector2D To = Context.Snap.Kind == ERoadSnapKind::Node
        ? Context.Snap.Position
        : Context.GuidedCursor();

    // A refused move simply does not happen, so the node stops following the cursor rather
    // than dragging a road shorter than the solver can trim. MoveNode notifies every
    // successful call, drag frame included (issue #77).
    Context.Target->MoveNode(DragNode, To);
}

void FEditTool::OnDragEnd(const FToolContext& Context)
{
    if (DragNode == INDEX_NONE || Context.Target == nullptr)
    {
        return;
    }

    DragNode = INDEX_NONE;
    Context.Target->EndInteractiveEdit(/*bKeep*/ true);
}

void FEditTool::OnDeactivate(const FToolContext& Context)
{
    if (DragNode != INDEX_NONE && Context.Target != nullptr)
    {
        Context.Target->EndInteractiveEdit(/*bKeep*/ true);
        DragNode = INDEX_NONE;
    }
}
```

And the preview:

```cpp
void FEditTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
    const URoadNetwork* Network = Context.Network();
    if (Network == nullptr)
    {
        return;
    }

    TArray<int32> Handles;
    GatherNodeHandles(Context, Handles);

    // EVERY GRABBABLE POINT, not just the one under the cursor: "what can I move" should be
    // answerable by looking. The one under the cursor is marked again as Hover, and the
    // overlay draws the two at different radii so neither simply overdraws the other.
    for (const int32 Index : Handles)
    {
        Sink.Marker(Network->GetNodes()[Index].Position, EPreviewStyle::Handle);
    }

    if (Context.Snap.Kind == ERoadSnapKind::Node && Handles.Contains(Context.Snap.Node.Index))
    {
        Sink.Marker(Context.Snap.Position, EPreviewStyle::Hover);
    }
}
```

Add the includes `Model/RoadNetwork.h`, `Model/RoadNode.h`, `Tool/RoadNaming.h`, `AirsideLog.h`.

- [ ] **Step 5: Take the drag out of `FRoadDrawTool`**

Delete from `RoadDrawTool.h`: the three `OnDrag*` overrides and the `DragNode` member (with its comment). Delete from the class comment the paragraph beginning "Dragging is deliberately NOT a state" — **and move it to `FEditTool`'s `DragNode`**, because it is a WHY comment that still explains the design and the refactor contract says such comments travel with their code. Amend the class summary from "place, chain, split, delete, and drag a node about" to "place, chain, split and delete".

Delete from `RoadDrawTool.cpp`: `OnDragBegin`, `OnDrag`, `OnDragEnd`, the `DragNode != INDEX_NONE` clause in `Tick`'s guard (keep the rest of the guard and amend its comment — it currently explains both cases), and the `DragNode` block in `OnDeactivate`.

In `BuildSession.cpp`, amend the Taxiway tooltip: drop `, drag a node to move it` and append ` Press M to edit placed nodes.`

- [ ] **Step 6: Build and run the full suite**

Expected: both new tests pass. **Check the run count has not dropped** — if a `RoadDrawTool` drag test existed, it will now fail rather than vanish; find it with `grep -rn "OnDragBegin" Plugins/Airside/Source/AirsideTests/` and rewrite it against `FEditTool` rather than deleting it.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(airside): Edit mode drags a node with placement's snapping

Takes the drag out of FRoadDrawTool, where any press-and-travel over a node
reshaped the road with no way to decline.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Guides for the drag

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/RoadGuideAnchor.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/RoadGuideAnchor.cpp`
- Modify: `Private/Tool/RoadDrawTool.cpp:336-380` — delegate the loop
- Modify: `Tool/EditTool.h/.cpp` — `DescribeGuideAnchor`
- Test: `EditToolTest.cpp` (append)

**Interfaces:**
- Produces: `namespace RoadGuideAnchor { void AddNodeCandidates(const URoadNetwork& Network, const FVector2D& Origin, int32 ExcludeIndex, FGuideAnchor& Out); }`

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEditModeDragOffersGuidesTest,
    "Airside.Tool.EditModeDragOffersGuides",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDragOffersGuidesTest::RunTest(const FString& Parameters)
{
    FAirsideTestWorld TestWorld;
    ARoadNetworkActor* Actor = TestWorld.Actor;
    if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

    // A road to drag the end of, and a lone node off to one side to line up WITH.
    const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
    const int32 B = Actor->PlaceNode(FVector2D(8000.0, 0.0));
    Actor->ConnectNodes(A, B);
    const int32 Landmark = Actor->PlaceNode(FVector2D(3000.0, 12000.0));
    const int32 LandmarkPartner = Actor->PlaceNode(FVector2D(9000.0, 12000.0));
    Actor->ConnectNodes(Landmark, LandmarkPartner);

    FBuildSession Session;
    Session.SelectTool(1);
    Session.SetGestureMode(EGestureMode::Edit);
    FBuildSessionTunables Tunables;

    Session.GetActiveTool()->OnDragBegin(
        Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));

    // Drag A to very nearly level with the landmark's row. A guide should claim it.
    const FVector2D NearRow(0.0, 12000.0 - 40.0);
    const FToolContext Context = Session.MakeContext(Actor, NearRow, Tunables, false, false);

    TestTrue(TEXT("a guide resolves for a drag, which it never did while the drag described "
                  "no anchor at all"),
        Context.Guide.bActive);

    // MEASURES THE DRAWING, NOT THE DESCRIBING. IBuildTool::WantsFreeStartGuides records a
    // tool that described an anchor, had a guide computed for it and drew nothing - which
    // showed the player exactly the same as having no guide.
    FEditGuideSink Sink;
    Session.GetActiveTool()->BuildPreview(Context, Sink);
    TestTrue(TEXT("and the dashed line to what it lined up with is actually drawn"),
        Sink.CountLines(EPreviewStyle::Guide) > 0);

    Session.GetActiveTool()->OnDragEnd(Context);
    return true;
}
```

Add a sink near the top of the file, uniquely named against the unity build:

```cpp
namespace
{
    /** Prefixed against the UNITY build - these test files share one translation unit. */
    struct FEditGuideSink : public IToolPreviewSink
    {
        TMap<EPreviewStyle, int32> Markers;
        TMap<EPreviewStyle, int32> Lines;

        virtual void Marker(const FVector2D&, EPreviewStyle S) override { Markers.FindOrAdd(S)++; }
        virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle S) override { Lines.FindOrAdd(S)++; }
        virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle S) override { Markers.FindOrAdd(S)++; }
        virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

        int32 CountMarkers(EPreviewStyle S) const { const int32* F = Markers.Find(S); return F ? *F : 0; }
        int32 CountLines(EPreviewStyle S) const { const int32* F = Lines.Find(S); return F ? *F : 0; }
    };
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: `Context.Guide.bActive` is false — `FEditTool` describes no anchor, so `MakeContext` resolves no guide.

- [ ] **Step 3: Extract the shared candidate loop**

`Public/Tool/RoadGuideAnchor.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Tool/SnapGuideChain.h"

class URoadNetwork;

/**
 * The network facts a guide anchor is built from, shared by every tool that has one.
 *
 * ONE LIST, not two. FRoadDrawTool's chaining anchor and FEditTool's drag anchor ask the
 * graph the identical question - "which live nodes near here could I line up with, and
 * which column does each belong to" - and a second copy would drift exactly as the four
 * private answers to RoadNaming::ReferenceOf's question did (see that header).
 */
namespace RoadGuideAnchor
{
    /**
     * Append one FGuidePoint per DISTINCT kind of segment meeting each live node within
     * the guide chain's search radius of Origin.
     *
     * ExcludeIndex is the node whose own lines pass through Origin - the one being extended
     * from, or the one being dragged. Both would otherwise always be in tolerance, and the
     * guide would say "you are level with yourself".
     *
     * A NODE BELONGS TO EVERY COLUMN THAT MEETS IT - ruled 2026-09-20, when Road became
     * Taxiway and ServiceRoad. Where a taxiway meets a service road the node is honestly
     * both, and picking a winner would make one of the two buttons lie about a junction
     * the player can see. A BARE node offers nothing: with no live segment on it there is
     * no kind to tag, and the caller's own Kind would be a guess about what the player
     * will attach to it.
     */
    AIRSIDE_API void AddNodeCandidates(const URoadNetwork& Network, const FVector2D& Origin,
        int32 ExcludeIndex, FGuideAnchor& Out);
}
```

Move the body from `RoadDrawTool.cpp:336-395` into `RoadGuideAnchor.cpp` **verbatim**, including every comment — they explain the rules above and the refactor contract says they travel with the code. Replace the loop in `FRoadDrawTool::DescribeGuideAnchor` with:

```cpp
    RoadGuideAnchor::AddNodeCandidates(*Network, Out.Origin, Pending, Out);
```

- [ ] **Step 4: Give the drag its anchor**

Add `virtual bool DescribeGuideAnchor(const URoadNetwork*, IRoadEditTarget*, FGuideAnchor&) const override;` to `EditTool.h`, and in the `.cpp`:

```cpp
bool FEditTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
    FGuideAnchor& Out) const
{
    if (Network == nullptr || DragNode == INDEX_NONE || !Network->GetNodes().IsValidIndex(DragNode))
    {
        // NOT IDLE-AND-FREE-START. The base's free start is for a gesture that has not begun;
        // with no drag there is no gesture at all, and offering positional guides for a
        // cursor that is about to grab something would guide the GRAB rather than the move.
        return false;
    }

    const FRoadNode& Dragged = Network->GetNodes()[DragNode];

    // FREE-START SHAPED, and for the real reason rather than by analogy: the thing moving IS
    // the node, so there is no fixed origin the way a chain has one. The driver puts the
    // cursor in Origin, and SnapGuide::Arbitrate then measures no direction from a point to
    // itself - so the angular candidates sit out unaided and exactly the positional ones
    // remain. See FGuideAnchor::bFreeStart.
    Out.bFreeStart = true;
    Out.Point = EDragPoint::Centreline;

    // THE WIDEST ARM'S PROFILE. A node's arms may differ, and the guide must be displaced by
    // the pavement that will actually be drawn - through ResolveProfileFor, the one resolver,
    // so a guide cannot disagree with what it is guiding.
    if (Target != nullptr)
    {
        double Widest = 0.0;
        for (const FRoadSegmentId Arm : Dragged.Incident)
        {
            const FRoadSegment* Segment = Network->GetSegment(Arm);
            const URoadProfile* Profile = Segment != nullptr ? Segment->Profile : nullptr;
            if (Profile != nullptr && Profile->GetTotalWidth() > Widest)
            {
                Widest = Profile->GetTotalWidth();
                Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
                Out.HalfWidthRight = Profile->GetHalfWidthRight();
            }
        }
    }

    // EXACTLY ONE ARM GIVES A DIRECTION TO HOLD. With two or more, no arm is "the" one, and
    // picking whichever is stored first would make the guide change with an edit nobody
    // connected to guides at all - the same rule and the same reason FRoadDrawTool's own
    // anchor gives about a junction.
    if (Dragged.Incident.Num() == 1)
    {
        const FRoadNodeId Far = Network->GetOtherEnd(Dragged.Incident[0], Network->NodeIdAt(DragNode));
        if (const FRoadNode* Other = Network->GetNode(Far))
        {
            const FVector2D Along = (Dragged.Position - Other->Position).GetSafeNormal();
            if (!Along.IsNearlyZero())
            {
                Out.Reference = Along;
                Out.ReferenceAt = Other->Position;
                Out.ReferenceName = TEXT("this road");
            }
        }
    }

    RoadGuideAnchor::AddNodeCandidates(*Network, Dragged.Position, DragNode, Out);
    return true;
}
```

- [ ] **Step 5: Draw the guide**

Append to `FEditTool::BuildPreview`:

```cpp
    // SAYING YES IS HALF THE WORK. IBuildTool::WantsFreeStartGuides records a tool that
    // described an anchor, had a guide computed and drew nothing - which showed the player
    // exactly what having no guide shows them.
    if (Context.Guide.bActive && DragNode != INDEX_NONE)
    {
        Sink.Line(Context.Guide.Against, Context.Guide.Point, EPreviewStyle::Guide);
        if (!Context.Guide.Label.IsEmpty())
        {
            Sink.Label(Context.Guide.Point, Context.Guide.Label, EPreviewStyle::Guide);
        }
    }
```

Check `SnapGuide::FResult`'s real field names in `Solve/GuideArbiter.h` before writing this — use whatever it calls the far end and the label.

- [ ] **Step 6: Build and run the full suite**

Expected: the new test passes and every existing guide test still does. The extraction touched `FRoadDrawTool`'s anchor, so `Airside.Tool.*Guide*` must all stay green.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(airside): a dragged node gets the same guides as a placed one

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: `MergeNodes` in the model

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/MergeNodesTest.cpp` (create)

**Interfaces:**
- Produces: `bool URoadNetwork::MergeNodes(FRoadNodeId Keep, FRoadNodeId Absorb);`

This is `Model/` — plain UObjects, no world needed. Per CLAUDE.md, write the failing test first and skip the repro entirely.

- [ ] **Step 1: Write the failing tests**

Create `MergeNodesTest.cpp` with five leaf tests. Each constructs a bare `URoadNetwork` with `NewObject<URoadNetwork>()`.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMergeCollapsesASharedSegmentTest,
    "Airside.Model.MergeCollapsesASharedSegment", ...)
// A-B joined. Merge A into B. Expect: the segment is gone, A is dead, B is alive,
// B has no incident arms. Reason: "a segment between two nodes that have become one
// node has no geometry - it would be a self-loop the solver has no answer for".

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMergeKeepsTheWiderDuplicateArmTest,
    "Airside.Model.MergeKeepsTheWiderDuplicateArm", ...)
// A-C (wide profile) and B-C (narrow). Merge A into B. Expect: exactly ONE live
// segment between B and C, and its Profile is the WIDE one.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMergeRepointsAnUnsharedArmTest,
    "Airside.Model.MergeRepointsAnUnsharedArm", ...)
// A-C and B-D. Merge A into B. Expect: B now reaches both C and D; A dead.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMergeKeepsAStraightSegmentStraightTest,
    "Airside.Model.MergeKeepsAStraightSegmentStraight", ...)
// A-C straight (Control == (A+C)/2). Merge A into B, which is elsewhere. Expect the
// repointed B-C segment still has Control == (B+C)/2 exactly. Reason: SetNodePosition
// shifts Control by half the endpoint displacement for this exact reason, and merge
// must match that precedent rather than invent one.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMergeSortsIncidentByBearingTest,
    "Airside.Model.MergeSortsIncidentByBearing", ...)
// Merge a node with arms into one with arms, then assert B.Incident is ascending by
// GetOutgoingTangent's atan2. Reason: URoadNetwork's contract is that Incident stays
// sorted by outgoing bearing, and the junction solver walks it assuming so - an arm in
// the wrong slot puts one road's geometry on another road's cut line.
```

Write each body out in full — do not leave the comment sketch above as the test.

- [ ] **Step 2: Build twice, run, verify they fail**

Expected: compile error — `MergeNodes` does not exist.

- [ ] **Step 3: Implement**

In `RoadNetwork.h`, beside `SetNodePosition`:

```cpp
    /**
     * Fold Absorb into Keep: every arm of Absorb becomes an arm of Keep, and Absorb dies.
     *
     * THE MERGE HAS NO VERB IN THE UI. Dropping one node on another is the gesture, exactly
     * as drawing within the snap radius reuses a node - see ERoadSnapKind::Node, "clicking
     * reuses it, which is how a junction is closed". This is that, for a node that already
     * exists.
     *
     * JUDGED WHOLE BEFORE ANYTHING MUTATES. A move-then-check would need an undo the drag
     * never asked for - the argument URoadEditFacade::MoveNode already makes for
     * NodeCornersFit. False leaves the graph exactly as it was found.
     *
     * Three cases per arm, and the middle one is the interesting one:
     *   - the arm's far end IS Keep    -> it collapses; the arm is removed
     *   - Keep already reaches that end -> the WIDER profile survives, the other is removed
     *   - otherwise                     -> the Absorb end is repointed to Keep
     *
     * WIDEST WINS by URoadProfile::GetTotalWidth, the same measure the width cycle reports;
     * on an exact tie the arm already on Keep survives, being the edit that touches less.
     * Ground geometry is sized for the largest aircraft admitted, so collapsing a stub must
     * never silently narrow a route something was cleared for.
     */
    bool MergeNodes(FRoadNodeId Keep, FRoadNodeId Absorb);
```

The body follows `SetNodePosition`'s shape: validate both slots, gather the plan, apply, then `SortIncident(Keep)` **and every surviving neighbour**, because a repointed arm changes its bearing at BOTH ends. Reuse `RemoveSegment` for the losers and write `Segment->A`/`Segment->B` directly for the repoint, shifting `Control` by half the endpoint displacement exactly as `SetNodePosition:183` does.

- [ ] **Step 4: Run and verify they pass**

- [ ] **Step 5: Check the tests measure something**

For `FMergeKeepsTheWiderDuplicateArmTest`, temporarily invert the width comparison and confirm it goes red. A green test that measures nothing has shipped here twice. Revert.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(airside): URoadNetwork::MergeNodes folds one node into another

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Merge on the drop

**Files:**
- Modify: `Tool/RoadEditTarget.h`, `Present/RoadEditFacade.h/.cpp`, `Present/RoadNetworkActor.h/.cpp`
- Modify: `Tool/EditTool.cpp` (`OnDragEnd`, `BuildPreview`)
- Modify: the two test doubles that implement `IRoadEditTarget`: `RunwayToolTest.cpp:75`, `TaxiwayWidthTest.cpp:52`
- Test: `EditToolTest.cpp` (append)

**Interfaces:**
- Produces: `IRoadEditTarget::MergeNodes(int32 KeepIndex, int32 AbsorbIndex)`.

- [ ] **Step 1: Write the failing test**

`Airside.Tool.EditModeDropOnNodeMerges` — build A-B and C-D, drag B onto C, assert the node count fell by one, that the survivor carries both arms, and that **one** undo restores both nodes (the merge joined the drag's interactive edit rather than opening its own).

- [ ] **Step 2: Run, verify it fails**

- [ ] **Step 3: Add the facade method**

Pure virtual on `IRoadEditTarget`; implement on `URoadEditFacade` through `CommitAndNotify` (it mutates pavement, so it notifies — unlike `SetIntermediateHoldingPosition`); forward from `ARoadNetworkActor` as a `UFUNCTION` beside `MoveNode`. Give the two test doubles `virtual bool MergeNodes(int32, int32) override { return false; }`.

- [ ] **Step 4: Merge on the drop**

In `FEditTool::OnDragEnd`, before clearing `DragNode`: if `Context.Snap.Kind == ERoadSnapKind::Node` and the snapped node is not `DragNode`, call `MergeNodes(Context.Snap.Node.Index, DragNode)`. Keep `Absorb` as the dragged node so the node the player aimed AT is the one that survives. Log both indices and the outcome.

- [ ] **Step 5: Preview the merge**

In `BuildPreview`, while dragging and snapped to another handle: mark the target `EPreviewStyle::Snap`, label it "merge", and mark any arm that would be discarded `EPreviewStyle::Doomed`.

- [ ] **Step 6: Build, run the full suite, commit**

---

### Task 8: The acceptance test — the slowdown goes away

**Files:**
- Test: `Plugins/Airside/Source/AirsideTests/Private/MergeNodesTest.cpp` (append)

- [ ] **Step 1: Write `Airside.Model.MergingClosePointsRemovesTheSpuriousSlowdown`**

Lay a taxiway run with two nodes a few metres apart making a tight corner. Run `FSpeedProfile` **over the whole route** and record the minimum. Merge the pair. Run it again. Assert the minimum rose.

Over the whole route and not per edge: four previous attempts shipped green by re-implementing `FSpeedProfile`'s rule one edge at a time, and the aircraft crabbed anyway. Find the real entry point with `grep -rn "FSpeedProfile" Plugins/Airside/Source/Airside/Public/` and call the same one the follower calls.

- [ ] **Step 2: Run, confirm it fails on the pre-merge network and passes after**

If it passes before the merge, the fixture's corner is not tight enough to trip the profile — tighten it until the pre-merge minimum is genuinely low, or the test measures nothing.

- [ ] **Step 3: Commit**

---

### Task 9: Runway thresholds

**Files:**
- Modify: `Private/Present/RoadEditFacade.cpp` (`MoveNode`)
- Test: `MergeNodesTest.cpp` or a new `EditGeometryTest.cpp`

- [ ] **Step 1:** Write `Airside.Model.MoveRunwayThresholdRefusesUnderMinimumLength` and `Airside.Model.DraggingAThresholdRedesignatesTheRunway` (the second asserts `RoadNaming::Describe` returns the new pair after a drag that swings the strip through 10 degrees).
- [ ] **Step 2:** Run, verify the first fails and the second passes already (the designator is derived — if it fails, the spec's reading was wrong and that is worth knowing before building anything on it).
- [ ] **Step 3:** Add the clause to `URoadEditFacade::MoveNode`, after `NodeCornersFit`: if the node is on a runway chain, refuse a position that pulls the strip under `GetMinimumRunwayLength()`.
- [ ] **Step 4:** Run, commit.

---

### Task 10: Apron corners

**Files:**
- Modify: `Tool/RoadEditTarget.h`, `Present/RoadEditFacade.h/.cpp`, `Present/RoadNetworkActor.h/.cpp`, the two test doubles
- Modify: `Tool/EditTool.h/.cpp` — a corner is not a node index, so it needs its own handle path
- Test: new leaf tests

- [ ] **Step 1:** Write `Airside.Model.MoveApronCornerRefusesSelfIntersection` and `Airside.Tool.EditModeDragsAnApronCorner`.
- [ ] **Step 2:** Run, verify they fail.
- [ ] **Step 3:** Add `MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To)` through the facade seam, refusing a move that makes the outline self-intersecting.
- [ ] **Step 4:** Give `FEditTool` a second drag target — `FEditHandle { enum class EKind { Node, ApronCorner }; int32 Owner; int32 Corner; }` — rather than overloading the node index. A corner is not a node and pretending otherwise is how a wrong index reaches `MoveNode`.
- [ ] **Step 5:** Assert the winding survives on an **engine-computed normal**, not a 2D signed area: CCW faces DOWN in Unreal.
- [ ] **Step 6:** Run, commit.

---

### Task 11: The bar and the key

**Files:**
- Modify: `Source/AirportMgr/BuildActions.cpp` (the Edit section)
- Modify: `Source/AirportMgr/RoadBuildController.h/.cpp` — `ToggleGestureMode`, `GetGestureMode`
- Test: `Source/AirportMgr/BuildActionsTest.cpp`

- [ ] **Step 1:** Write a test asserting `FindAction(TEXT("edit.editmode"))` exists, its key is `EKeys::M`, it is in `EActionSection::Edit`, and `IsEnabled` is false when the lit tool's `EditHandles` is `None`.
- [ ] **Step 2:** Run, verify it fails.
- [ ] **Step 3:** Add the row. It goes **through the table**, which is what makes the key binding, the bar button and the startup banner one list:

```cpp
        // EDIT MODE: the second axis. Not a tool - it does not go in the Tools section and
        // has no number - because it changes what EVERY tool's gesture means. M rather than
        // E: Q/E is camera turn (see UpdateView), and M is the key a Cities player already
        // has in their fingers from Move It.
        //
        // GREYED when the lit tool exposes nothing, so the bar answers "why can I not edit
        // this" rather than letting the toggle light over a mode that does nothing.
        Out.Add(Make(TEXT("edit.editmode"), EActionSection::Edit, LOCTEXT("EditMode", "Edit"),
            EKeys::M, false,
            [](ARoadBuildController& C) { C.ToggleGestureMode(); },
            [](const ARoadBuildController& C) { return C.GetGestureMode() == EGestureMode::Edit; },
            [](const ARoadBuildController& C) { return C.ActiveToolHasEditHandles(); }));
```

- [ ] **Step 4:** Add the three controller methods, forwarding to the session.
- [ ] **Step 5:** Run the full suite. Commit.

---

## Final verification

Tests are necessary and not sufficient — a graph change once passed 348 tests and put kilometre-wide arcs across the apron.

- [ ] Full suite from the worktree; read `N run, N failed, N crashed`.
- [ ] `./Tools/Check-Architecture.ps1` clean.
- [ ] `grep -c "UE_LOG(" ` on every touched file, against `origin/main`. A refactor that dropped one says which and why in the PR.
- [ ] PIE: press `M` with Taxiway lit — the toggle lights and every taxiway node shows a handle.
- [ ] PIE: drag a node — the dashed guide appears and the node lands on it.
- [ ] PIE: drop a node on another — check `Saved/Logs/AirportMgr.log` for the merge line, and look at the junction for a seam. The surface model welds bitwise; a broken merge shows as a crack.
- [ ] `python Tools/Mcp.py shot out.png` of the merged junction.
- [ ] PIE: press `M` with Select lit — the toggle is greyed.
- [ ] Drag a runway threshold; read the designator off the surface.
