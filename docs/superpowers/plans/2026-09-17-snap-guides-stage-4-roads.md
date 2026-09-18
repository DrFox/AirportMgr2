# Snap guides, stage 4: the road tools

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drawing a taxiway or a service road gets the same guides the plot gesture already
has - square to the road you are extending, parallel to the taxiway beside you, level with
that junction - so every source built in stages 1 to 3 starts earning its keep.

**Architecture:** `FRoadDrawTool` declares an anchor and reads `GuidedCursor()`. Nothing in
the chain, the arbiter or the drawing changes. Two things make this more than wiring: the
tool needs the NETWORK to find the segment it is extending, so `DescribeGuideAnchor` gains
one; and a real snap must beat a guide, because joining an existing node is a statement about
the graph while an alignment is only an aid.

**Tech Stack:** UE 5.8.2 C++, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-17-snap-guides-design.md` - what §8 lists as stage 5.

**STAGE ORDER CHANGED, 2026-09-17.** §8 puts Offset fourth and road drawing fifth. They are
swapped, by decision: Offset is "the gap a neighbouring parallel ROAD keeps", so its only
consumer is road drawing - built in §8's order, the Offset stage would ship arbitration with
nothing on screen, failing §8's own test that each stage "leaves the branch usable on its
own". Road drawing first gives every guide in stages 1-3 a second consumer immediately, and
Offset then lands with its consumer already there and its §3 meaning intact.

**Stacked on:** `feature/snap-guides-toggles` (PR #148), itself on #147, itself on #146. None
merged. This branch is `feature/snap-guides-roads` and its PR targets
`feature/snap-guides-toggles`.

## Global Constraints

- **Worktree.** Every command runs from `C:\repos\AirportMgr2_snap-roads`. Do not `cd` to
  `C:\repos\AirportMgr2` nor to the three worktrees holding PRs #146-#148 open.
- **Build.**

  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_snap-roads\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```

  Check the literal line `Result: Succeeded`. **It exits 0 even when it fails.** That has
  happened four times across stages 1-3. Never read the exit code.
- **Tests.**

  ```
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-roads\AirportMgr.uproject"
  ```

  Read `N test(s) run, N failed, N crashed`; **crashed must be 0**, and a crash is reported by
  the runner's started-vs-completed diff and by nothing else. **Baseline is 375**; record the
  real number before Task 1.
- **A new `.cpp` sometimes needs two builds.** **This module is a UNITY build** - prefix any
  file-local helper whose name is at all common, as stage 2 learned with `SegmentEnds`.
- **`Solve/` includes `CoreMinimal.h` and `Solve/` only**; `Tool/` may include `Solve/` and
  `Model/`, never `Present/`.
- **Never `git checkout --` a file whose task is not yet committed.**
- **No string replacement without asserting it matched exactly once.**
- **Commits:** no `Co-Authored-By` trailer.

## What stages 1-3 already provide

```cpp
// Tool/RoadBuildTool.h
struct FToolContext
{
    FVector2D Cursor;                 // the RAW plane hit
    FRoadSnapResult Snap;             // what the snap chain made of it
    SnapGuide::FResult Guide;         // what the guide chain made of it
    bool bSuspendGuides = false;      // Alt
    FVector2D GuidedCursor() const;   // Guide.Point when active, else Cursor
};
struct IBuildTool
{
    virtual bool DescribeGuideAnchor(FGuideAnchor& Out) const { return false; }   // CHANGES in task 1
    // ...
};

// Tool/SnapGuideChain.h
struct FGuidePoint { FVector2D At; FString Name; };
struct FGuideAnchor
{
    FVector2D Origin;         // the fixed point the moving point swings around
    FVector2D Reference;      // the direction being extended; zero for none
    FVector2D ReferenceAt;    // where Extending's dashed line is drawn TO
    FString ReferenceName;    // "the frontage" - composed into "square to the frontage"
    TArray<FGuidePoint> AlignTo;   // points the moving point may line up WITH
};

// Tool/RoadSnap.h
enum class ERoadSnapKind : uint8 { Free, Node, Segment };
struct FRoadSnapResult { ERoadSnapKind Kind; FVector2D Position; FRoadNodeId Node; ... };
```

## The road tool, as it actually is (verified on this branch)

```cpp
// Tool/RoadDrawTool.h
struct AIRSIDE_API IRoadDrawState
{
    virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext&) = 0;
    virtual int32 GetPendingNode() const { return INDEX_NONE; }
};
class AIRSIDE_API FRoadIdleState : public IRoadDrawState { ... };          // nothing pending
class AIRSIDE_API FRoadChainingState : public IRoadDrawState
{   // holds `int32 From` and returns it from GetPendingNode()
};
class AIRSIDE_API FRoadDrawTool : public IBuildTool
{
public:
    int32 GetPendingNode() const;      // forwards to State; INDEX_NONE when idle
    // ...
private:
    TUniquePtr<IRoadDrawState> State;
    int32 DragNode = INDEX_NONE;
};

// Private/Tool/RoadDrawTool.cpp:49 - where a click puts a node:
return Context.Target->PlaceNode(Context.Snap.Position);
```

**`FRoadChainingState` holds only `From`.** It has no memory of the node before it, so the
direction of the segment being extended is not on the tool - it is in the graph. That is why
Task 1 exists.

## Decisions this plan takes

1. **`DescribeGuideAnchor` gains a `const URoadNetwork*`.** Stage 1 made it context-free
   because "the driver calls it WHILE BUILDING a context" - that argument was about the
   CONTEXT, not the network, and `MakeContext` has already resolved the network two lines
   above the call. The road tool cannot find its incoming segment without it, and caching the
   previous node on the chaining state would be a second copy of something the graph already
   knows.
2. **A SNAP BEATS A GUIDE.** A road click places at `Context.Snap.Position`. When the snap
   claimed a node or a segment the player is attaching to something real, and a guide that
   overrode it would make closing a junction impossible while any guide was live. So the
   guided point is used only when `Snap.Kind == ERoadSnapKind::Free`.
3. **The road's `AlignTo` is the nearby road NODES**, named by position rather than by index -
   "level with that junction" is what the player sees, and a node has no other name. Bounded
   by `FTuning::SearchRadiusUu`, like every other local source.
4. **Only the chaining endpoint is guided, not node drags.** `DragNode` moves an existing node
   and has its own rules (`RoadPlacement::Validate` against both its neighbours); guiding it
   is a separate question and a separate stage's worth of testing. Named here so its absence
   is a decision rather than an oversight.

## File Structure

| File | Responsibility |
|---|---|
| `Public/Tool/RoadBuildTool.h` | `DescribeGuideAnchor` gains the network parameter |
| `Private/Tool/BuildSession.cpp` | passes the network it already resolved |
| `Public/Tool/PlotPlaceTool.h` + `.cpp` | signature updated; behaviour unchanged |
| `Public/Tool/RoadDrawTool.h` + `Private/Tool/RoadDrawTool.cpp` | declares the anchor, reads the guide |
| `AirsideTests/Private/RoadGuideTest.cpp` (new) | the anchor, the consumption, snap-beats-guide |

---

### Task 1: The hook learns about the network

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp`

**Interfaces:**
- Consumes: `FGuideAnchor`, `FBuildSession::MakeContext` (stages 1-3).
- Produces: `virtual bool IBuildTool::DescribeGuideAnchor(const URoadNetwork* Network,
  FGuideAnchor& Out) const { return false; }`.

- [ ] **Step 1: Record the baseline**

Run the full test line. Expected `375 test(s) run, 0 failed, 0 crashed`. Write it down.

- [ ] **Step 2: Widen the hook**

In `Public/Tool/RoadBuildTool.h`, replace the declaration and extend its comment:

```cpp
	/**
	 * What this tool is dragging, and against what, for the guide chain. False means "no
	 * gesture is in progress", and the driver then resolves no guide at all.
	 *
	 * CONST AND STATELESS, read off what the tool has already pinned.
	 *
	 * IT TAKES THE NETWORK BUT NOT THE CONTEXT, and the distinction is not fussiness: the
	 * driver calls this WHILE BUILDING a context, so there is none to pass - but it has
	 * already resolved the network two lines earlier, and a tool that must ask the graph what
	 * it is extending (FRoadDrawTool does; its chaining state holds only the node it draws
	 * FROM) would otherwise have to keep its own copy of something the graph already knows.
	 * Null when there is no network yet, which is the first click of a session.
	 *
	 * RETURNS BOOL rather than setting a flag inside FGuideAnchor, so callers branch on the
	 * return - CLAUDE.md's rule about honouring anything that fills an out-parameter.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, FGuideAnchor& Out) const
	{
		return false;
	}
```

`URoadNetwork` is already forward-declared in this header (`FToolContext::Network()` returns
one); confirm with a grep before adding another.

- [ ] **Step 3: Pass what the session already has**

In `Private/Tool/BuildSession.cpp`, in `MakeContext`, the call becomes:

```cpp
	if (!bSuspendGuides && Tool != nullptr && Network != nullptr
		&& Tool->DescribeGuideAnchor(Network, Anchor))
```

`Network` is the local already resolved for `ResolveSnap` a few lines above - do not resolve
it twice.

- [ ] **Step 4: Update the one existing implementor**

In `Public/Tool/PlotPlaceTool.h` and `Private/Tool/PlotPlaceTool.cpp`, change the signature to
match. **The plot tool ignores the parameter** - it has everything it needs on itself - so
name it and say so, rather than leaving an unnamed parameter a reader has to wonder about:

```cpp
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, FGuideAnchor& Out) const override;
```

```cpp
bool FPlotPlaceTool::DescribeGuideAnchor(const URoadNetwork* Network, FGuideAnchor& Out) const
{
	// THE NETWORK IS UNUSED HERE, deliberately: this gesture's reference is its own frontage
	// and its points are its own pinned corners, both of which live on the tool. FRoadDrawTool
	// is the implementor that needs the graph - see IBuildTool::DescribeGuideAnchor.
```

and leave the rest of the body exactly as it is.

- [ ] **Step 5: Update the registry test**

In `Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp`, the idle-anchor walk
calls the hook. Pass null, and say why that is the right argument:

```cpp
			FGuideAnchor Anchor;
			TestFalse(
				FString::Printf(TEXT("tool %d offers no guide anchor while it is idle"), Index),
				// NULL NETWORK ON PURPOSE: an idle tool must decline before it looks at the
				// graph at all, which is also the first-click-of-a-session case.
				Active->DescribeGuideAnchor(nullptr, Anchor));
```

- [ ] **Step 6: Build**

Expect `Result: Succeeded`. A changed virtual signature, so this is a full build.

- [ ] **Step 7: Run the whole suite**

Expected: baseline unchanged (375), 0 failed, 0 crashed. **No test count change and no
behaviour change** - this task only widens a signature. If a guide test fails here, the
parameter has been threaded wrongly rather than the behaviour altered.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp
git commit -m "refactor(tool): the guide anchor hook can ask the graph what it is extending"
```

---

### Task 2: The road tool declares what it is extending

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadDrawTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp`

**Interfaces:**
- Consumes: `DescribeGuideAnchor(const URoadNetwork*, FGuideAnchor&)` (Task 1);
  `FRoadDrawTool::GetPendingNode()`.
- Produces: `FRoadDrawTool::DescribeGuideAnchor` override.

- [ ] **Step 1: Declare it**

In `Public/Tool/RoadDrawTool.h`, in `FRoadDrawTool`'s public section after `OnReselect`:

```cpp
	/**
	 * The node the chain is drawing FROM, and the direction of the segment already arriving
	 * there - which is design §3's "the incoming segment's direction, and its perpendicular".
	 *
	 * ASKS THE GRAPH, because FRoadChainingState holds only the node it draws from and not
	 * the one before it. See IBuildTool::DescribeGuideAnchor on why the network is a
	 * parameter.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, FGuideAnchor& Out) const override;
```

- [ ] **Step 2: Implement it**

In `Private/Tool/RoadDrawTool.cpp`, add `#include "Model/RoadNetwork.h"`,
`#include "Model/RoadNode.h"` and `#include "Solve/GuideArbiter.h"` if they are not already
there, then:

```cpp
bool FRoadDrawTool::DescribeGuideAnchor(const URoadNetwork* Network, FGuideAnchor& Out) const
{
	// NOTHING PENDING MEANS NOTHING TO EXTEND. The first click of a chain has no direction to
	// speak of, and a guide offered there would be squaring to an edge that does not exist.
	const int32 Pending = GetPendingNode();
	if (Network == nullptr || Pending == INDEX_NONE)
	{
		return false;
	}

	const FRoadNodeId FromId = Network->NodeIdAt(Pending);
	const FRoadNode* From = Network->GetNode(FromId);
	if (From == nullptr)
	{
		return false;
	}

	Out.Origin = From->Position;

	// THE SEGMENT ALREADY ARRIVING AT THE PENDING NODE. With exactly one incident segment the
	// answer is unambiguous - that is the road being extended. At a junction there are several
	// and none of them is "the" incoming one, so no reference is offered rather than an
	// arbitrary one: a guide that squared to whichever segment happened to be stored first
	// would change with an unrelated edit.
	int32 Incident = 0;
	FVector2D Along = FVector2D::ZeroVector;
	FVector2D OtherEnd = FVector2D::ZeroVector;

	const TArray<FRoadSegment>& Segments = Network->GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || (Segment.A != FromId && Segment.B != FromId))
		{
			continue;
		}

		const FRoadNode* Far = Network->GetNode(Segment.A == FromId ? Segment.B : Segment.A);
		if (Far == nullptr)
		{
			continue;
		}

		++Incident;
		Along = (From->Position - Far->Position).GetSafeNormal();
		OtherEnd = Far->Position;
	}

	if (Incident == 1 && !Along.IsNearlyZero())
	{
		Out.Reference = Along;
		Out.ReferenceAt = OtherEnd;
		Out.ReferenceName = TEXT("this road");
	}

	// EVERY NODE IN REACH IS SOMETHING TO LINE UP WITH - "level with that junction" is the
	// thing a player squinting at a taxiway layout actually wants. Named by nothing but their
	// role, because a node has no other name; the dashed line drawn to it is what says WHICH.
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;
	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FRoadNode& Node = Nodes[Index];

		// NOT THE NODE BEING EXTENDED FROM: its own lines pass through the origin, so both
		// would always be in tolerance and the guide would say "you are level with yourself".
		if (Index == Pending
			|| FVector2D::DistSquared(Node.Position, Out.Origin) > Reach * Reach)
		{
			continue;
		}
		Out.AlignTo.Add({ Node.Position, TEXT("that node") });
	}

	return true;
}
```

`FRoadNode` has `bAlive` (`Model/RoadNode.h:21`) - verified, not assumed - so the `AlignTo`
loop must skip dead nodes the way the segment loop above it does. Add `|| !Node.bAlive` to that
loop's `continue` condition: a deleted node still occupies its slot, and offering one would
draw a dashed line to a junction the player has removed.

- [ ] **Step 3: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A road-drawing session with the taxiway tool selected, found by id. */
	struct FRoadGesture
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;
		IBuildTool* Tool = nullptr;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}

		FRoadDrawTool* Road() const { return static_cast<FRoadDrawTool*>(Tool); }
	};

	bool StartRoadGesture(FRoadGesture& Out)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }

		int32 Taxiway = INDEX_NONE;
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(TEXT("Taxiway"))) { Taxiway = Index; }
		}
		if (Taxiway == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Session.SelectTool(Taxiway);
		Out.Tool = Out.Session.GetActiveTool();
		return Out.Tool != nullptr;
	}
}

/**
 * A CHAIN EXTENDS THE ROAD IT IS ALREADY DRAWING. Two clicks make one segment; the third
 * click's guide must be square to THAT segment, which is design §3's Extending row applied
 * to the tool it was written for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAnchorExtendsTheSegmentBehindItTest,
	"Airside.Tool.RoadAnchorExtendsTheSegmentBehindIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAnchorExtendsTheSegmentBehindItTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	// NOTHING PENDING YET: the first click has no segment behind it to extend.
	FGuideAnchor Idle;
	TestFalse(TEXT("an idle road tool offers no anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.TestWorld.Actor->Network, Idle));

	// Two clicks, west to east: one segment, and the chain now pends at its east end.
	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("with a segment behind it, the tool offers an anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.TestWorld.Actor->Network, Anchor)))
	{
		return false;
	}

	TestTrue(TEXT("the anchor swings around the node the chain is drawing from"),
		Anchor.Origin.Equals(FVector2D(6000.0, 0.0), 1.0));
	TestTrue(TEXT("and extends the segment already arriving there"),
		FMath::IsNearlyEqual(FMath::Abs(Anchor.Reference.X), 1.0, 1.0e-6));
	TestTrue(TEXT("with the dashed line pointing back down that segment"),
		Anchor.ReferenceAt.Equals(FVector2D(0.0, 0.0), 1.0));

	// THE OTHER END OF THE SEGMENT IS SOMETHING TO LINE UP WITH, and the node being extended
	// FROM is not - its own lines pass through the origin, so it would always be in tolerance.
	TestTrue(TEXT("the far node is offered as an alignment point"),
		Anchor.AlignTo.ContainsByPredicate([](const FGuidePoint& P)
			{ return P.At.Equals(FVector2D(0.0, 0.0), 1.0); }));
	TestFalse(TEXT("and the node being drawn from is not"),
		Anchor.AlignTo.ContainsByPredicate([](const FGuidePoint& P)
			{ return P.At.Equals(FVector2D(6000.0, 0.0), 1.0); }));

	return true;
}

#endif
```

- [ ] **Step 4: Build, then run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-roads\AirportMgr.uproject" -Filter Airside.Tool.RoadAnchorExtends
```

Expected: `1 test(s) run, 0 failed, 0 crashed`. A new `.cpp` may need two builds.

- [ ] **Step 5: Run the whole suite**

Expected: baseline + 1, 0 failed, 0 crashed. **Nothing consumes the anchor yet**, so every
road test must still pass unchanged - if one moves here, the anchor is being read by something
it should not be.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadDrawTool.h Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp
git commit -m "feat(tool): the road tool says what it is extending"
```

---

### Task 3: The road follows the guide, unless the snap claimed something

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp`

**Interfaces:**
- Consumes: `FToolContext::GuidedCursor()`, `FToolContext::Snap` (stages 1-3).
- Produces: a file-local `FRoadSnapResult RoadGuidedSnap(const FToolContext&)`.

- [ ] **Step 1: Write the one rule down, once - as a SNAP, not a point**

The mesh ghost takes a whole `FRoadSnapResult` (`RoadDrawTool.cpp:417`,
`Target->UpdateGhost(Pending, Context.Snap, ...)`), so the rule has to live on the struct. A
bare point would leave the ghost road drawn from the raw snap while the click placed a node on
the guide - the preview and the click disagreeing, which this codebase treats as the one
unacceptable class of bug.

In `Private/Tool/RoadDrawTool.cpp`, in its anonymous namespace:

```cpp
	/**
	 * The snap as this tool should ACT on it: its position moved onto the guide when the chain
	 * claimed nothing.
	 *
	 * A SNAP BEATS A GUIDE. When the chain claimed a node or a segment the player is attaching
	 * to something REAL - closing a junction, splitting a run - and that is a statement about
	 * the graph, where an alignment is only an aid. A guide allowed to override it would make a
	 * junction impossible to close while any guide was live, which is far worse than a guide
	 * occasionally not applying.
	 *
	 * RETURNS THE WHOLE RESULT so the ghost, the placement judgement and the click all take the
	 * same value. Handing some of them a position and others the raw snap is how a preview comes
	 * to promise what a click does not do.
	 *
	 * PREFIXED because this module is a unity build and "GuidedSnap" is exactly the name a
	 * second tool would also choose - see stage 2's SegmentEnds collision.
	 */
	FRoadSnapResult RoadGuidedSnap(const FToolContext& Context)
	{
		FRoadSnapResult Guided = Context.Snap;
		if (Guided.Kind == ERoadSnapKind::Free)
		{
			Guided.Position = Context.GuidedCursor();
		}
		return Guided;
	}
```

- [ ] **Step 2: Use it at the four sites that decide where the road goes**

Grep `Context.Snap` in `RoadDrawTool.cpp` and change exactly these, asserting each replacement
matches once. There are ten `Snap.Position` reads; six are the SNAP MARKER, the Ctrl-remove
preview or a segment split, and must NOT change.

1. **`RoadDrawTool.cpp:49`** - `return Context.Target->PlaceNode(Context.Snap.Position);`
   becomes `return Context.Target->PlaceNode(RoadGuidedSnap(Context).Position);`
   Where the node actually lands.
2. **`RoadDrawTool.cpp:417`** - `Context.Target->UpdateGhost(Pending, Context.Snap, ...)`
   becomes `Context.Target->UpdateGhost(Pending, RoadGuidedSnap(Context), ...)`
   The mesh ghost: the road the player watches while dragging.
3. **`RoadDrawTool.cpp:550`** - `Sink.Marker(Context.Snap.Position, EPreviewStyle::Pending);`
   becomes `Sink.Marker(RoadGuidedSnap(Context).Position, EPreviewStyle::Pending);`
   Its own comment calls this "where the click lands on the PLANE", so it must be.
4. **`RoadDrawTool.cpp:174`** - `RoadPlacement::Validate(*Context.Network(), FromId,
   Context.Snap, Context.Limits)` becomes `..., RoadGuidedSnap(Context), ...`.

   **THE FOURTH IS THE ONE THAT IS EASY TO GET BACKWARDS.** `Validate`'s third parameter is
   named `To` - it is WHERE THE SEGMENT GOES, not what it attaches to - so it must judge the
   guided position. Judging the raw cursor while the click builds on the guide would let the
   preview call a placement fine and the click produce a segment shorter than
   `FRoadPlacementLimits` allows.

Leave every other `Context.Snap` read alone. `SplitSegment` (lines 45 and 260) runs only when
the snap claimed a segment, where `RoadGuidedSnap` returns the snap unchanged anyway.

- [ ] **Step 3: Draw the guide**

A road that snaps but shows nothing is half the feature. In `FRoadDrawTool::BuildPreview`,
directly after the Pending marker from step 2:

```cpp
	// THE DASHED LINE TO WHAT IT IS LINED UP WITH, one per winner - the same emission the plot
	// gesture makes, and deliberately the same shape: two tools drawing one meaning two different
	// ways would be presentation drifting apart inside the plugin.
	//
	// FROM THE POINT THE CLICK WOULD TAKE, so the line touches the marker above rather than
	// floating beside it.
	if (Context.Guide.bActive)
	{
		const FVector2D Moving = RoadGuidedSnap(Context).Position;
		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// At the line's MIDPOINT: two labels at the moving point overprint, and the plugin has
			// no camera to offset them by a readable number of pixels. Design section 6.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}
```

Add `#include "Solve/GuideArbiter.h"` if it is not already there.

- [ ] **Step 4: Write the failing test**

Append to `RoadGuideTest.cpp`, before the final `#endif`:

```cpp
/**
 * THE THIRD CLICK SQUARES TO THE SECOND SEGMENT, and lands exactly on the perpendicular
 * rather than merely near it. This is the assertion that fails if the tool never reads
 * FToolContext::Guide at all - every other test in this file passes on a tool that ignores it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCornerFollowsTheGuideTest,
	"Airside.Tool.RoadCornerFollowsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCornerFollowsTheGuideTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	// A third click dragged nearly square to the run just drawn: 2000 out, 60 along, about
	// 1.7 degrees off the perpendicular and inside the 7-degree tolerance.
	const FVector2D NearSquare(6060.0, 2000.0);
	const FToolContext Guided = Gesture.At(NearSquare);
	if (!TestTrue(TEXT("the driver resolved a guide for the third click"),
		Guided.Guide.bActive))
	{
		return false;
	}

	Gesture.Tool->OnClick(Guided);

	// THE NODE LANDS EXACTLY SQUARE, which is what the guide is for. Read back from the graph
	// rather than from the tool, so this measures what was BUILT.
	const URoadNetwork* Network = Gesture.TestWorld.Actor->Network;
	if (!TestTrue(TEXT("a network"), Network != nullptr)) { return false; }

	const int32 Pending = Gesture.Road()->GetPendingNode();
	const FRoadNode* Placed = Network->GetNode(Network->NodeIdAt(Pending));
	if (!TestNotNull(TEXT("the third click placed a node"), Placed)) { return false; }

	TestTrue(TEXT("the new node is exactly square to the segment behind it"),
		FMath::IsNearlyEqual(Placed->Position.X, 6000.0, 1.0e-6));
	TestFalse(TEXT("and therefore not where the raw cursor was"),
		FMath::IsNearlyEqual(NearSquare.X, 6000.0, 1.0e-6));

	return true;
}

/**
 * A SNAP BEATS A GUIDE. Closing a junction on an existing node must win over any alignment, or
 * the player could never join two roads while a guide happened to be live.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSnapBeatsTheGuideTest,
	"Airside.Tool.RoadSnapBeatsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSnapBeatsTheGuideTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	// An existing node to close onto, placed deliberately OFF the square: if the guide won,
	// the click would land at x = 6000 instead of on this node.
	IRoadEditTarget* Target = Gesture.TestWorld.Actor;
	const int32 Existing = Target->PlaceNode(FVector2D(6080.0, 2000.0));
	const URoadNetwork* Network = Gesture.TestWorld.Actor->Network;
	if (!TestTrue(TEXT("a network"), Network != nullptr)) { return false; }

	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	// A cursor ON the existing node: near enough to square that the guide is live, and near
	// enough to the node that the snap claims it.
	const FToolContext OnNode = Gesture.At(FVector2D(6080.0, 2000.0));
	TestEqual(TEXT("the snap claimed the existing node"),
		static_cast<int32>(OnNode.Snap.Kind), static_cast<int32>(ERoadSnapKind::Node));

	const int32 NodesBefore = Network->GetNodes().Num();
	Gesture.Tool->OnClick(OnNode);

	// NO NEW NODE: the click reused the one that was there, which is how a junction closes.
	TestEqual(TEXT("closing on an existing node makes no new one"),
		Network->GetNodes().Num(), NodesBefore);

	const FRoadNode* Reused = Network->GetNode(Network->NodeIdAt(Existing));
	if (!TestNotNull(TEXT("the existing node survives"), Reused)) { return false; }
	TestTrue(TEXT("and it did not move to satisfy the guide"),
		Reused->Position.Equals(FVector2D(6080.0, 2000.0), 1.0e-6));

	return true;
}
```

- [ ] **Step 5: Build, then run both new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-roads\AirportMgr.uproject" -Filter Airside.Tool.Road
```

Expected: every `Airside.Tool.Road*` test passes, old and new.

- [ ] **Step 6: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed. **`Airside.Tool.RoadDraw` is the one to watch**:
it drives the same tool through clicks and may sit near an alignment. If it fails, read it -
the fix may be its geometry, or it may be that the guide is applying where a snap should have
won.

- [ ] **Step 7: Prove the consumption is real**

Change `RoadGuidedSnap` to `return Context.Snap;` unconditionally. Build. Run
`-Filter Airside.Tool.RoadCornerFollowsTheGuide`. Expected: `1 failed`, on "the new node is
exactly square to the segment behind it". Restore the body **by hand**, rebuild, rerun,
expect `0 failed`.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp
git commit -m "feat(tool): a road follows its guide, unless the snap claimed something real"
```

---

### Task 4: Record the swap, and judge it in PIE

**Files:**
- Modify: `docs/superpowers/specs/2026-09-17-snap-guides-design.md`

- [ ] **Step 1: Record the reordering and the stage in §8**

Replace §8's entries 4 and 5 with:

```markdown
4. ~~**Road drawing** as the second consumer.~~ **Done 2026-09-17.** Brought forward from
   fifth: Offset is "the gap a neighbouring parallel road keeps", so road drawing is its only
   consumer, and doing Offset first would have shipped arbitration with nothing on screen -
   failing this section's own test that a stage stands on its own. `DescribeGuideAnchor` gained
   a `const URoadNetwork*` so a tool can ask the graph what it is extending; `FRoadChainingState`
   holds only the node it draws FROM. A SNAP BEATS A GUIDE: a road click uses the guided point
   only when the snap is Free, or closing a junction would become impossible while a guide was
   live.
5. **Offset**, the distance family, which needs its own arbitration pass. Now last, with road
   drawing already in place to consume it.
```

- [ ] **Step 2: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-09-17-snap-guides-design.md
git commit -m "docs(spec): road drawing comes before Offset, and it landed"
```

- [ ] **Step 4: Judge it in PIE**

1. Press `1` (Taxiway). Click twice to lay a run.
2. Drag the third click slowly through square to that run: a dark cyan dashed line back down
   the segment, labelled "square to this road", and the node landing exactly square.
3. Drag it near an existing junction instead: the snap should win - the ghost should sit on
   that node, not on the alignment.
4. Hold **Alt**: the guide should vanish for that click.

What to report: whether guides while drawing roads feel helpful or fussy, whether "that node"
reads sensibly as a label, and whether `SearchRadiusUu` at 100 m offers too many alignment
points on a busy field.

---

## Unresolved questions

1. **`AlignTo` names every node "that node".** With several in reach the labels are identical
   and only the dashed line distinguishes them. If PIE says that is confusing, nodes need
   describing by something - their junction degree, or the road they belong to.
2. **Node DRAGS are not guided** (decision 4). Dragging an existing node into line with its
   neighbours is arguably the case that wants this most.
3. **Offset is now last.** If it slips, stage 3's greyed `snap.offset` button stays greyed -
   which is honest, but it is a promise on screen with no delivery date.
