# Entity Inspector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the Route tool; add a default-state Select tool, a world-free facts layer over agents and stands, a one-button Depart, and an inspector panel with Depart and Follow.

**Architecture:** `FSelectTool : IBuildTool` at registry index 0 writes an `FSelection` the session carries and the panel reads. Aircraft are picked in screen space by the controller and handed to the tool as `FToolContext::HoverAgent`; stands by `FindEntityAt`. `Model/InspectFacts` turns `FRoadAgent` + `URoadNetwork` into plain structs the panel renders. Depart = `DeparturePlanner::PlanAny` (shortest admitted taxi over every runway) + the existing `RedirectAgent`, exposed as `UGroundTraffic::DepartAgent` and forwarded up. `UInspectorWidget` (game module) polls facts each tick; Depart and Follow are two rows of the one `BuildActions()` table.

**Tech Stack:** UE 5.8.2 C++, UMG (C++ base, Blueprint restyle), UE automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), `Tools/Run-AirsideTests.ps1`, `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-07-entity-inspector-design.md`

## Global Constraints

- Layering (Check-Architecture rule 1): `Model/` and `Solve/` never include `Build/|Tool/|Present/|Entities/`; `Tool/` never includes `Present/`; `Build/` never includes `Present/|Tool/`.
- One `DEFINE_LOG_CATEGORY_STATIC` name per module (unity build). Airside test helpers in anonymous namespaces get a unique prefix (`Insp`, `PlanAny`, `SelTool`) for the same reason.
- Every `UE_LOG` in deleted code is either moved or named in the PR. Count `UE_LOG(` in `Plugins/Airside/Source/Airside` and `Source/AirportMgr` before Task 1 and after Task 9.
- New `UPROPERTY`/`UCLASS`/vtable classes need a full build with the editor CLOSED:
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
  Expected last lines: `Result: Succeeded`. If it says "Unable to build while Live Coding is active", the editor is open: check nothing is unsaved, close it, retry.
- Tests: `./Tools/Run-AirsideTests.ps1 -Filter <Prefix>`; read its `N test(s) run, N failed, N crashed` line, never the exit code. A filter that matches nothing reports `0 test(s) run` - that is a failure of the filter, not a pass.
- Comments explain WHY and the rejected alternative. Doc comments touch their declarations.
- Commit messages: `type(scope): imperative` with a body; no Co-Authored-By trailer (user CLAUDE.md). Branch `feature/entity-inspector` (exists; spec committed on it).
- Spec amendment recorded in Task 1: `AirframeFor` is DELETED, not moved to `Model/` - it includes `Entities/` and `Content/`, which `Model/` may not, and the Route tool was its only caller.

---

## File map

| File | Responsibility |
|---|---|
| `Plugins/Airside/Source/Airside/Public/Tool/RouteTool.h` + `Private/Tool/RouteTool.cpp` | DELETE |
| `Plugins/Airside/Source/AirsideTests/Private/RouteToolTest.cpp` | DELETE |
| `Plugins/Airside/Source/Airside/Public/Tool/Selection.h` | `ESelectionKind`, `FSelection` (plain structs) |
| `Plugins/Airside/Source/Airside/Public/Tool/SelectTool.h` + `Private/Tool/SelectTool.cpp` | `FSelectTool : IBuildTool` |
| `Plugins/Airside/Source/Airside/Public/Tool/ScreenPick.h` + `Private/Tool/ScreenPick.cpp` | `ScreenPick::NearestWithin` |
| `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` | `FToolContext::HoverAgent`, `FToolContext::Selection`, `EPreviewStyle::Hover/Selected` |
| `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h` | `GetGroundTraffic()` default virtual |
| `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h` + `.cpp` | registry index 0, `Selection` member, cancel returns to Select, clears on tool switch |
| `Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h` + `Private/Model/InspectFacts.cpp` | `FAgentFacts`, `FStandFacts`, `InspectFacts::DescribeAgent/DescribeStand/IcaoCodeForWingspan` |
| `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` + `.cpp` | `FindEntityIndexByPoseNode` |
| `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` | `FAirframe::TypeCode` |
| `Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h` | `Airframe()` fills `TypeCode` |
| `Plugins/Airside/Source/Airside/Public/Model/DeparturePlanner.h` + `.cpp` | `PlanAny`, `EDepartureRefusal::NotParked` |
| `Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h` + `Private/Model/GroundTraffic.cpp` | `DepartAgent` |
| `Plugins/Airside/Source/Airside/Public/Present/AirsideTraffic.h` + `.cpp` | `DepartAgent`, `GetAgentView` forwarders |
| `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` + `.cpp` | `DepartAgent`, `GetAgentView`, `GetGroundTraffic` |
| `Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEdModeCommands.h` + `Private/RoadBuildEdModeCommands.cpp` | `SelectEntities` command replaces `FindRoutes`, list reordered |
| `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp` | comment only: why cancel stays in place there |
| `Source/AirportMgr/RoadBuildController.h` + `.cpp` | hover projection, selection accessors, `DepartSelected`, Follow retarget, inspector creation |
| `Source/AirportMgr/BuildActions.h` + `.cpp` | `EActionSection::Selection`, `Depart`, `Follow` |
| `Source/AirportMgr/RoadBuildHUD.h` + `.cpp` | `HoverColour`, `SelectedColour` |
| `Source/AirportMgr/InspectorWidget.h` + `.cpp` | `UInspectorWidget` |
| Tests | `SelectToolTest.cpp`, `ScreenPickTest.cpp`, `InspectFactsTest.cpp`, `PlanAnyTest.cpp`, `DepartAgentTest.cpp` (AirsideTests); `InspectorWidgetTest.cpp` (AirportMgr); edits to `BuildSessionTest.cpp`, `GuidelineOverlayTest.cpp`, `ToolCursorTest.cpp`, `BuildActionsTest.cpp`, `BuildBarWidgetTest.cpp` |

---

### Task 0: Baseline

**Files:** none changed.

- [ ] **Step 1: Confirm branch and record the log-line count**

```powershell
git status --short; git branch --show-current
(Select-String -Path Plugins/Airside/Source/Airside/**/*.cpp,Plugins/Airside/Source/Airside/**/*.h,Source/AirportMgr/*.cpp,Source/AirportMgr/*.h -Pattern 'UE_LOG\(' -AllMatches | Measure-Object).Count
```
Expected: branch `feature/entity-inspector`; only `M Content/Maps/M_Starter.umap` (the user's, never staged). Write the count into your notes; the PR body needs before/after.

- [ ] **Step 2: Confirm the editor is closed**

```powershell
Get-Process UnrealEditor -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle
```
Expected: nothing. If a process is listed, tell the user before any build; do not kill it.

---

### Task 1: Retire the Route tool

**Files:**
- Delete: `Plugins/Airside/Source/Airside/Public/Tool/RouteTool.h`, `Plugins/Airside/Source/Airside/Private/Tool/RouteTool.cpp`, `Plugins/Airside/Source/AirsideTests/Private/RouteToolTest.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp` (registry row + include), `Plugins/Airside/Source/AirsideTests/Private/GuidelineOverlayTest.cpp:11,208-290`, `Plugins/Airside/Source/AirsideTests/Private/ToolCursorTest.cpp:11,110-175`, `Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEdModeCommands.h:32`, `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEdModeCommands.cpp`, `Source/AirportMgr/RoadBuildController.cpp:300-304` (comment only), `Source/AirportMgr/RoadBuildHUD.cpp:52` (comment only), `docs/superpowers/specs/2026-09-07-entity-inspector-design.md` §7

**Interfaces:**
- Produces: a registry with no key 4 (Task 2 fills it). `EPreviewStyle::Route` stays.

- [ ] **Step 1: Delete the three files**

```powershell
git rm Plugins/Airside/Source/Airside/Public/Tool/RouteTool.h Plugins/Airside/Source/Airside/Private/Tool/RouteTool.cpp Plugins/Airside/Source/AirsideTests/Private/RouteToolTest.cpp
```

The two `UE_LOG` lines in RouteTool.cpp that go: `UE_LOG(LogAirside, Log, TEXT("%s"), *DeparturePlanner::Describe(Departure))` (re-emitted by `DepartAgent` in Task 6) and nothing else - verify with `git show HEAD:Plugins/Airside/Source/Airside/Private/Tool/RouteTool.cpp | Select-String UE_LOG`. Name both in the PR.

- [ ] **Step 2: Remove the registry row and include**

In `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`: delete `#include "Tool/RouteTool.h"` and the line
```cpp
		{ EKeys::Four,  LOCTEXT("Route",     "Route"),     [] { return MakeUnique<FRouteTool>(); } },
```
Update the comment above the table: `1 through 6 then 8` becomes `1 through 3, 5, 6 then 8 (4 is the Select tool, added at index 0 - see FSelectTool)`. Also update the `FToolRegistration` doc comment in `BuildSession.h:24-25` ("in key order 1..6: Road, Apron, Stand, Route, Guideline, Runway") to "Select (4), Taxiway (1), Apron (2), Stand (3), Guidelines (5), Runway (6), Holding point (8)".

- [ ] **Step 3: Rewrite the two route-tool cases in GuidelineOverlayTest**

Replace `#include "Tool/RouteTool.h"` with `#include "Tool/RoadDrawTool.h"`. Replace block 4 (`// 4. ONE EMITTER ...` through its closing brace) with:

```cpp
	// 4. ONE EMITTER. No tool draws the graph itself, or selecting it draws every edge twice
	//    - and the overlay stops being the single place that decides what the routing graph
	//    looks like. The taxiway tool is the one that would most plausibly want to: it
	//    builds the thing the graph is derived from.
	{
		ARoadNetworkActor* Fresh = OverlayFixture();
		if (TestNotNull(TEXT("second fixture built"), Fresh))
		{
			FToolContext Context;
			Context.Target = Fresh;
			Context.SnapRadius = 150.0;

			FRoadSnapResult NoSnap;
			NoSnap.Position = FVector2D(50000.0, 50000.0);   // far from anything
			Context.SetCursor(NoSnap.Position, NoSnap);

			FRoadDrawTool Tool;
			FOverlaySink Sink;
			Tool.BuildPreview(Context, Sink);

			TestEqual(TEXT("the taxiway tool draws no guideline lines of its own"),
				Sink.CountLines(EPreviewStyle::Guideline), 0);
			TestEqual(TEXT("nor guideline node markers"),
				Sink.CountMarkers(EPreviewStyle::Guideline), 0);
		}
	}
```
Delete block 5 entirely (`// 5. But the route tool keeps its OWN preview ...` through its closing brace): it tested guideline-node hover, which no tool does now.

- [ ] **Step 4: Rewrite the route-tool case in ToolCursorTest**

Replace `#include "Tool/RouteTool.h"` with `#include "Tool/BuildSession.h"`. Replace from `FRouteTool Tool;` (line ~131) through the end of the enclosing block (the `}` before `return true;`) with:

```cpp
		// THE MECHANISM, not a tool that happens to use it: FBuildSession::MakeContext must
		// hand a tool the RAW hit as Cursor with the snap carried beside it. The tool that
		// used to demonstrate the bug (the route tool picking a guideline node the snap had
		// moved the cursor off) is gone; the contract it relied on is asserted directly.
		{
			FBuildSession Session;
			FBuildSessionTunables Tunables;
			Tunables.Snap = Settings;
			const FToolContext Context = Session.MakeContext(Actor, Hover, Tunables, false, false);

			TestTrue(TEXT("Cursor is the raw hover point"), Context.Cursor.Equals(Hover, 1e-6));
			TestTrue(TEXT("the snap beside it still claims the junction"),
				Context.Snap.Kind == ERoadSnapKind::Node);
			TestFalse(TEXT("and the two differ - folding one into the other is the bug"),
				Context.Snap.Position.Equals(Hover, 1.0));
		}
```
If `FCursorPreviewSink` is now unused, delete it too (the compiler warns on an unused local type only if instantiated; an unused struct definition is fine to leave, but remove it to keep the file honest).

- [ ] **Step 5: Editor commands - remove FindRoutes for now**

`RoadBuildEdModeCommands.h`: delete `TSharedPtr<FUICommandInfo> FindRoutes;`. `RoadBuildEdModeCommands.cpp`: delete the `UI_COMMAND(FindRoutes, ...)` block and remove `FindRoutes` from `ToolCommandsInOrder()`. (Task 2 adds `SelectEntities` at the front.)

- [ ] **Step 6: Fix the two comments that name FRouteTool**

`RoadBuildController.cpp:300`: change `// The SAME resolver FRouteTool falls back to` to `// The SAME resolver every dispatch falls back to`. `RoadBuildHUD.cpp:52`: change `FRouteTool` to `the old route tool`.

- [ ] **Step 7: Spec amendment**

In the spec §7, replace the sentence starting `AirframeFor` moves to `Model/DeparturePlanner.h` ... with:

```
*Amended 2026-09-07 (Task 1):* `AirframeFor` is DELETED with the tool, not moved to
`Model/`. It includes `Entities/AircraftType.h` and `Content/AirsideSettings.h`, both of
which `Model/` is forbidden (Check-Architecture rule 1), and the Route tool was its only
caller. Its test (`Airside.Tool.RouteTool.DefaultAirframe`) pinned that the fallback
agreed with `UAirsideSettings::ResolveDefaultAirframe`; with one resolver and no second
caller there is nothing left to agree. Check-Architecture rule 4 (one Piper fallback site)
still holds.
```

- [ ] **Step 8: Build**

Run the build line. Expected `Result: Succeeded`. Fix any remaining reference to `FRouteTool` or `AirframeFor` the compiler finds (grep: `Select-String -Path Plugins,Source -Pattern 'FRouteTool|AirframeFor' -Recurse` should return only the spec/plan docs).

- [ ] **Step 9: Test**

```powershell
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool
```
Expected: `N test(s) run, 0 failed, 0 crashed` with N ≥ 10 (BuildSession, GuidelineOverlay, ToolCursor and the draw-tool tests among them).

- [ ] **Step 10: Commit**

```powershell
git add -A Plugins Source docs/superpowers/specs
git commit -m "refactor(tool): retire the route tool

Departures come from the inspector (this branch), arrivals from Land. AirframeFor deleted
with its only caller; spec §7 amended (Model/ may not include Entities/). Two UE_LOG lines
gone: the planner Describe line returns in DepartAgent. Editor FindRoutes command removed."
```
(Do NOT `git add Content/Maps/M_Starter.umap`.)

---

### Task 2: Selection, Select tool, registry index 0

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/Selection.h`, `Plugins/Airside/Source/Airside/Public/Tool/SelectTool.h`, `Plugins/Airside/Source/Airside/Private/Tool/SelectTool.cpp`, `Plugins/Airside/Source/AirsideTests/Private/SelectToolTest.cpp`
- Modify: `Tool/RoadBuildTool.h` (context fields, styles), `Tool/RoadEditTarget.h` (`GetGroundTraffic`), `Tool/BuildSession.h/.cpp`, `Present/RoadNetworkActor.h/.cpp` (`GetGroundTraffic` override), `AirsideTests/Private/BuildSessionTest.cpp`, `AirsideEditor/.../RoadBuildEdModeCommands.h/.cpp`, `AirsideEditor/Private/RoadBuildEditorTool.cpp:441-450` (comment), `Source/AirportMgr/RoadBuildHUD.h/.cpp` (colours), spec §3.4 amendment

**Interfaces:**
- Produces:
  ```cpp
  enum class ESelectionKind : uint8 { None, Aircraft, Stand };
  struct FSelection { ESelectionKind Kind = ESelectionKind::None; int32 Id = 0; bool IsSet() const; void Clear(); };
  struct FToolContext { ...; int32 HoverAgent = 0; FSelection* Selection = nullptr; };
  class IRoadEditTarget { virtual const UGroundTraffic* GetGroundTraffic() const { return nullptr; } };
  class FBuildSession { const FSelection& GetSelection() const; FToolContext MakeContext(..., bool bInsertModifier, int32 HoverAgent = 0) const; };
  enum class EPreviewStyle { ..., Hover, Selected };
  ```
- Consumes: `UGroundTraffic::FindAgent(int32)`, `FRoadAgent::LastMotion.Position`, `IRoadEditTarget::FindEntityAt(FVector2D, double)`, `URoadNetwork::GetEntities()`.

- [ ] **Step 1: Write the failing tests**

`Plugins/Airside/Source/AirsideTests/Private/SelectToolTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/SelectTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FSelToolSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		virtual void Marker(const FVector2D&, EPreviewStyle Style) override { Markers.FindOrAdd(Style)++; }
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}
		int32 Count(EPreviewStyle S) const { const int32* N = Markers.Find(S); return N ? *N : 0; }
	};

	/** An actor with a network, one authored edge, one stand at (50000, 0) and one van
	 *  dispatched along the edge. Prefixed SelTool against the unity build. */
	struct FSelToolFixture
	{
		ARoadNetworkActor* Actor = nullptr;
		int32 StandIndex = INDEX_NONE;
		int32 AgentId = 0;
		FVector2D StandAt = FVector2D(50000.0, 0.0);
	};

	FSelToolFixture SelToolBuild(UWorld* World)
	{
		FSelToolFixture F;
		F.Actor = World->SpawnActor<ARoadNetworkActor>();
		if (F.Actor == nullptr) { return F; }
		F.Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
		URoadNetwork& Net = *F.Actor->Network;

		const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
		const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));

		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FEntityInstanceId Placed = Net.PlaceEntity(Stand, Stand->Anchors, F.StandAt, 0.0);
		F.StandIndex = Placed.Index;

		FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::GroundVehicle;
		FAirframe Van = UAirsideSettings::ResolveDefaultAirframe();
		Van.Climb = FClimbPerformance();
		F.Actor->DispatchAgent(RouteSearch::Find(Net, Q), Van, ETraversalClass::GroundVehicle);
		F.AgentId = F.Actor->GetTraffic()->GetNewestAgentId();
		return F;
	}

	FToolContext SelToolContext(ARoadNetworkActor* Actor, FSelection& Selection, FVector2D Cursor, int32 HoverAgent)
	{
		FToolContext C;
		C.Target = Actor;
		C.SnapRadius = 400.0;
		FRoadSnapResult NoSnap; NoSnap.Position = Cursor;
		C.SetCursor(Cursor, NoSnap);
		C.HoverAgent = HoverAgent;
		C.Selection = &Selection;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSelectToolRegistryTest,
	"Airside.Tool.SelectTool.IsDefaultState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSelectToolRegistryTest::RunTest(const FString& Parameters)
{
	// CITIES-STYLE DEFAULT STATE (spec §2): the session opens in Select, and cancelling an
	// idle build tool comes back to it. Asserted on the registry and the session, not on
	// a controller, because both drivers read exactly these.
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	if (!TestTrue(TEXT("the registry has entries"), Registry.Num() > 0)) { return false; }
	TestTrue(TEXT("registry index 0 is Select"), Registry[0].Name.ToString() == TEXT("Select"));
	TestTrue(TEXT("Select took the route tool's key"), Registry[0].Key == EKeys::Four);

	FBuildSession Session;
	TestEqual(TEXT("a fresh session starts in Select"), Session.GetActiveToolIndex(), 0);

	// Cancel from an IDLE build tool returns to Select; cancel in Select stays.
	Session.SelectTool(1);
	TestEqual(TEXT("tool 1 is active"), Session.GetActiveToolIndex(), 1);
	FToolContext Empty;
	Session.CancelActiveGesture(Empty);
	TestEqual(TEXT("cancelling an idle build tool returns to Select"), Session.GetActiveToolIndex(), 0);
	Session.CancelActiveGesture(Empty);
	TestEqual(TEXT("cancelling in Select stays in Select"), Session.GetActiveToolIndex(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSelectToolPickTest,
	"Airside.Tool.SelectTool.Picks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSelectToolPickTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	FSelToolFixture F = SelToolBuild(World);
	if (!TestNotNull(TEXT("fixture actor"), F.Actor)) { return false; }
	if (!TestTrue(TEXT("a stand was placed"), F.StandIndex != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("an agent was dispatched"), F.AgentId > 0)) { return false; }

	FSelectTool Tool;
	FSelection Sel;

	// 1. A hovered aircraft wins, wherever the plane cursor is - even over the stand.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, F.AgentId));
	TestTrue(TEXT("hover agent selects the aircraft"), Sel.Kind == ESelectionKind::Aircraft && Sel.Id == F.AgentId);

	// 2. No hover, cursor on the stand: the stand.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestTrue(TEXT("a click on a stand selects it"), Sel.Kind == ESelectionKind::Stand && Sel.Id == F.StandIndex);

	// 3. Preview names the selection with the Selected style.
	{
		FSelToolSink Sink;
		Tool.BuildPreview(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), 0), Sink);
		TestEqual(TEXT("the selected stand gets one Selected marker"), Sink.Count(EPreviewStyle::Selected), 1);
		TestEqual(TEXT("nothing hovered, so no Hover marker"), Sink.Count(EPreviewStyle::Hover), 0);
	}
	{
		FSelToolSink Sink;
		Tool.BuildPreview(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), F.AgentId), Sink);
		TestEqual(TEXT("a hovered aircraft gets one Hover marker"), Sink.Count(EPreviewStyle::Hover), 1);
	}

	// 4. Empty click clears; cancel clears.
	Tool.OnClick(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), 0));
	TestFalse(TEXT("a click on nothing clears the selection"), Sel.IsSet());
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	Tool.OnCancel(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestFalse(TEXT("cancel clears the selection"), Sel.IsSet());

	// 5. Tick drops a selection whose agent is gone.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, F.AgentId));
	F.Actor->GetTraffic()->RetireAgent(F.AgentId);
	Tool.Tick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestFalse(TEXT("a retired agent is no longer selected"), Sel.IsSet());

	// 6. Switching to a build tool clears the SESSION's selection.
	{
		FBuildSession Session;
		FBuildSessionTunables T;
		FToolContext C = Session.MakeContext(F.Actor, F.StandAt, T, false, false, 0);
		Session.GetActiveTool()->OnClick(C);
		TestTrue(TEXT("the session's selection is the stand"), Session.GetSelection().Kind == ESelectionKind::Stand);
		Session.SelectTool(1, C);
		TestFalse(TEXT("activating a build tool clears the selection"), Session.GetSelection().IsSet());
	}
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

```powershell
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.SelectTool
```
Expected: the build step inside the script fails (no `Tool/SelectTool.h`). That is the failing state.

- [ ] **Step 3: `Tool/Selection.h`**

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * What the player has clicked on. AN ENUM AND AN ID, not two optional ids: an aircraft and
 * a stand can never both be selected, so the state that would need a tie-break rule is not
 * representable (CLAUDE.md, "a phase is an enum, never a set of bools"). Vehicle is added
 * here the day one exists; the panel that reads this does not change shape for it.
 */
enum class ESelectionKind : uint8
{
	None,
	/** Id is a UGroundTraffic agent id. */
	Aircraft,
	/** Id is an entity INDEX into URoadNetwork::GetEntities() - what FindEntityAt returns. */
	Stand
};

/**
 * Lives on FBuildSession, WRITTEN ONLY by FSelectTool (through FToolContext::Selection), read
 * by the inspector panel and the HUD. Plain struct, not a USTRUCT: it is runtime UI state
 * that never reaches disk or Blueprint.
 */
struct FSelection
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = 0;

	bool IsSet() const { return Kind != ESelectionKind::None; }
	void Clear() { Kind = ESelectionKind::None; Id = 0; }
};
```

- [ ] **Step 4: `FToolContext` and `EPreviewStyle` additions**

In `Tool/RoadBuildTool.h`, add `#include "Tool/Selection.h"` and, after `bInsertModifier`:

```cpp
	/**
	 * The agent under the cursor IN SCREEN SPACE, or 0. Filled by the driver, which owns the
	 * camera: an aircraft on final is 2000 uu up and a road-plane cursor lands on the grass
	 * beneath it, so the plane hit can never say "that aeroplane". The tool takes the id and
	 * never sees a projection - which is what keeps FSelectTool in the plugin.
	 */
	int32 HoverAgent = 0;

	/**
	 * Where a selection is recorded. Points at FBuildSession::Selection; null in a driver or
	 * test that has no session, in which case the Select tool selects nothing and says so.
	 * A pointer rather than a copy because the tool WRITES it, and the panel reads the
	 * session's copy, so there must be exactly one.
	 */
	FSelection* Selection = nullptr;
```

In `EPreviewStyle`, after `IntermediateHoldingPosition`:

```cpp
	/** What a click would select right now: the pickable under the cursor. */
	Hover,

	/** What IS selected. Drawn every frame the selection stands, so it can be found again. */
	Selected,
```

- [ ] **Step 5: `IRoadEditTarget::GetGroundTraffic`**

In `Tool/RoadEditTarget.h`, forward-declare `class UGroundTraffic;` at the top and add after `GetNetwork()`:

```cpp
	/**
	 * The agents, read-only, for a tool that asks about them (Select). Model/, so Tool/ may
	 * see it; the Present-layer UAirsideTraffic stays invisible here.
	 *
	 * A DEFAULT rather than pure virtual: URoadEditFacade implements this interface too and
	 * genuinely has no traffic, and every other implementer forwards to the one that does.
	 */
	virtual const UGroundTraffic* GetGroundTraffic() const { return nullptr; }
```

In `Present/RoadNetworkActor.h`, next to `GetNetwork()` override:
```cpp
	virtual const UGroundTraffic* GetGroundTraffic() const override;
```
In `RoadNetworkActor.cpp` (near the other Traffic forwarders, ~line 476):
```cpp
const UGroundTraffic* ARoadNetworkActor::GetGroundTraffic() const
{
	return Traffic != nullptr ? Traffic->GetModel() : nullptr;
}
```
Add `#include "Model/GroundTraffic.h"` to the .cpp if not present.

- [ ] **Step 6: `Tool/SelectTool.h`**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * The default state: click a thing, see its facts, press a verb. Registry index 0, so the
 * session opens here and an idle build tool's cancel returns here (FBuildSession).
 *
 * The one tool that BUILDS NOTHING, inheriting that title from the Route tool it replaced.
 * The Route tool asked the airport a question by making it drive; this asks by pointing.
 *
 * PICKING IS SPLIT BY WHAT CAN BE HIT ON THE PLANE. Stands are on the plane and FindEntityAt
 * finds them from Context.Cursor. Aircraft may not be - one on final is 2000 uu up - so the
 * driver projects them and hands the nearest in Context.HoverAgent; this tool never sees a
 * camera. Aircraft beats stand: a parked aircraft covers its stand and the smaller target
 * should win, or the stand would be the only thing selectable once an aircraft is on it.
 *
 * Stateless apart from what it writes to Context.Selection: the selection belongs to the
 * session so the panel can read it after this tool has been deactivated by a build tool
 * (which clears it) or reactivated (which does not).
 */
class AIRSIDE_API FSelectTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;
	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void Tick(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/** Idle when nothing is selected, so a second cancel falls through to the driver. */
	virtual bool IsIdle() const override { return !bHasSelection; }

private:
	/** Mirror of Context.Selection->IsSet() from the last call, because IsIdle takes no context. */
	bool bHasSelection = false;

	/** Road-plane position of a selection, or false when it no longer exists. */
	static bool PositionOf(const FToolContext& Context, ESelectionKind Kind, int32 Id, FVector2D& Out);
};
```

- [ ] **Step 7: `Tool/SelectTool.cpp`**

```cpp
#include "Tool/SelectTool.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FSelectTool::GetDisplayName() const
{
	return LOCTEXT("SelectTool", "Select");
}

bool FSelectTool::PositionOf(const FToolContext& Context, ESelectionKind Kind, int32 Id, FVector2D& Out)
{
	if (Context.Target == nullptr)
	{
		return false;
	}
	switch (Kind)
	{
	case ESelectionKind::Aircraft:
	{
		const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic();
		const FRoadAgent* Agent = Traffic != nullptr ? Traffic->FindAgent(Id) : nullptr;
		if (Agent == nullptr)
		{
			return false;
		}
		// The ROAD-PLANE position: for an airborne aircraft this is its ground track, and
		// the ring the HUD draws sits under it. Right for every aircraft on the ground,
		// which is where anything selectable long enough to matter is.
		Out = Agent->LastMotion.Position;
		return true;
	}
	case ESelectionKind::Stand:
	{
		const URoadNetwork* Network = Context.Target->GetNetwork();
		if (Network == nullptr || !Network->GetEntities().IsValidIndex(Id) || !Network->GetEntities()[Id].bAlive)
		{
			return false;
		}
		Out = Network->GetEntities()[Id].Position;
		return true;
	}
	default:
		return false;
	}
}

void FSelectTool::OnClick(const FToolContext& Context)
{
	if (Context.Selection == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("Select: click with no selection to write to - the driver built a context without one."));
		return;
	}
	FSelection& Sel = *Context.Selection;

	if (Context.HoverAgent != 0)
	{
		Sel.Kind = ESelectionKind::Aircraft;
		Sel.Id = Context.HoverAgent;
	}
	else if (Context.Target != nullptr)
	{
		const int32 Stand = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Stand != INDEX_NONE)
		{
			Sel.Kind = ESelectionKind::Stand;
			Sel.Id = Stand;
		}
		else
		{
			// A click on nothing deselects, as it does in every Cities-style game: the
			// panel closing is how the player knows the click registered.
			Sel.Clear();
		}
	}
	bHasSelection = Sel.IsSet();
	UE_LOG(LogAirside, Log, TEXT("Select: %s %d"),
		Sel.Kind == ESelectionKind::Aircraft ? TEXT("aircraft") : Sel.Kind == ESelectionKind::Stand ? TEXT("stand") : TEXT("nothing"),
		Sel.Id);
}

void FSelectTool::OnCancel(const FToolContext& Context)
{
	if (Context.Selection != nullptr)
	{
		Context.Selection->Clear();
	}
	bHasSelection = false;
}

void FSelectTool::Tick(const FToolContext& Context)
{
	if (Context.Selection == nullptr || !Context.Selection->IsSet())
	{
		bHasSelection = false;
		return;
	}
	// Polled, not subscribed: a tool has no delegate lifetime to manage, and this runs
	// every frame anyway for the preview. A selected aircraft that has flown away or a
	// stand that was deleted under another tool must not leave the panel showing a ghost.
	FVector2D Unused;
	if (!PositionOf(Context, Context.Selection->Kind, Context.Selection->Id, Unused))
	{
		UE_LOG(LogAirside, Log, TEXT("Select: selection %d no longer exists; cleared."), Context.Selection->Id);
		Context.Selection->Clear();
	}
	bHasSelection = Context.Selection->IsSet();
}

void FSelectTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	FVector2D At;
	if (Context.HoverAgent != 0 && PositionOf(Context, ESelectionKind::Aircraft, Context.HoverAgent, At))
	{
		Sink.Marker(At, EPreviewStyle::Hover);
	}
	else if (Context.Target != nullptr)
	{
		const int32 Stand = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Stand != INDEX_NONE && PositionOf(Context, ESelectionKind::Stand, Stand, At))
		{
			Sink.Marker(At, EPreviewStyle::Hover);
		}
	}

	if (Context.Selection != nullptr && Context.Selection->IsSet()
		&& PositionOf(Context, Context.Selection->Kind, Context.Selection->Id, At))
	{
		Sink.Marker(At, EPreviewStyle::Selected);

		// The selected aircraft's remaining route, in the style the Route tool used: the one
		// useful picture that tool drew, kept.
		if (Context.Selection->Kind == ESelectionKind::Aircraft)
		{
			const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic();
			const FRoadAgent* Agent = Traffic != nullptr ? Traffic->FindAgent(Context.Selection->Id) : nullptr;
			if (Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing)
			{
				const TArray<FVector2D>& Poly = Agent->Follower.Plan.Polyline;
				for (int32 I = 1; I < Poly.Num(); ++I)
				{
					Sink.Line(Poly[I - 1], Poly[I], EPreviewStyle::Route);
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
```
Check `FRouteFollower` exposes `Plan` (grep `Plan;` in `Model/RouteFollower.h`); if the member is named differently, use that name.

- [ ] **Step 8: Session changes**

`Tool/BuildSession.h`: add `#include "Tool/Selection.h"`; in the class, public:
```cpp
	/** What the Select tool has picked. Read by the panel and the HUD; written only through
	 *  FToolContext::Selection, which MakeContext points here. */
	const FSelection& GetSelection() const { return Selection; }
```
Change `MakeContext`'s signature to add `int32 HoverAgent = 0` as the last parameter, and its doc: "HoverAgent is the driver's screen-space pick, 0 when none - see FToolContext::HoverAgent." Private:
```cpp
	/**
	 * mutable: MakeContext is const (see FBuildSessionTunables for why that was fought for)
	 * and must hand out a pointer the tool can write through. The selection is tool state
	 * parked on the session so it outlives the tool being active.
	 */
	mutable FSelection Selection;
```
Update the class doc's "The first tool starts active" to "Index 0 - Select - starts active".

`Tool/BuildSession.cpp`: add `#include "Tool/SelectTool.h"`; first registry row:
```cpp
		// INDEX 0 IS THE DEFAULT STATE (spec 2026-09-07-entity-inspector §2): the session
		// opens here and CancelActiveGesture returns here. Key 4 because that was the route
		// tool's, whose slot this fills; the printed keys 1-3 keep their meaning.
		{ EKeys::Four,  LOCTEXT("Select",    "Select"),    [] { return MakeUnique<FSelectTool>(); } },
```
In `SelectTool(Index, ...)`, after `ActiveTool = Index;`:
```cpp
	// A build tool is modal over the airport, not over a thing in it: the selection closes
	// with the panel when one opens, and does not come back when it is cancelled.
	if (Index != 0)
	{
		Selection.Clear();
	}
```
In `MakeContext`, set `Context.HoverAgent = HoverAgent; Context.Selection = &Selection;`.
Replace `CancelActiveGesture`:
```cpp
void FBuildSession::CancelActiveGesture(const FToolContext& Context)
{
	IBuildTool* Tool = GetActiveTool();
	if (Tool == nullptr)
	{
		return;
	}
	if (!Tool->IsIdle())
	{
		Tool->OnCancel(Context);
		return;
	}
	// Idle, and not in Select: cancel means "put the tool down". Two cancels from mid-gesture
	// reach Select; one from an idle build tool does. In Select itself an idle cancel is a
	// no-op rather than a toggle to anything.
	if (ActiveTool != 0)
	{
		SelectTool(0, Context);
	}
}
```

- [ ] **Step 9: Editor: `SelectEntities` command and cancel through the session**

`RoadBuildEdModeCommands.h`: add `TSharedPtr<FUICommandInfo> SelectEntities;` before `DrawRoads`. `.cpp` `RegisterCommands()`: before `DrawRoads`:
```cpp
	// INDEX 0 in ToolCommandsInOrder, matching ToolRegistry(): the Select tool. Key 4 as at
	// runtime. The label must equal the registry's "Select" exactly - see Enter's check.
	UI_COMMAND(SelectEntities, "Select", "Click an aircraft or a stand to inspect it. Escape deselects.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Four));
```
`ToolCommandsInOrder()`: `return { SelectEntities, DrawRoads, DrawAprons, PlaceStands, DrawGuidelines, PlaceRunways, PlaceHoldingPoint };`. While here, the labels "Aprons" and "Stands" already fail Enter's identity check against "Apron"/"Stand" - change the two UI_COMMAND labels to `"Apron"` and `"Stand"` so the check passes. Say so in the commit.

`RoadBuildEditorTool.cpp:450`: LEAVE `Tool->OnCancel(MakeHoverContext());` as it is, and add above it: `// Not Session.CancelActiveGesture: each editor tool owns a session pinned to ONE palette entry (URoadBuildEditorToolBuilder::ToolIndex), so "return to Select" here would run the Select tool under a palette button that still says Taxiway. In the editor, Escape ends the gesture and the palette changes tools.` Record this in the spec §3.4 as a dated amendment: "*Amended 2026-09-07 (Task 2):* the editor mode keeps cancel-in-place; return-to-Select is a play-driver behaviour, for the reason in RoadBuildEditorTool.cpp."

- [ ] **Step 10: HUD colours**

`RoadBuildHUD.h`, after `RouteColour`:
```cpp
	/** The pickable under the cursor - what a click would select. */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview")
	FLinearColor HoverColour = FLinearColor(1.0f, 1.0f, 1.0f);

	/** The current selection. Warm, so it reads against the cyan route and grey nodes. */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview")
	FLinearColor SelectedColour = FLinearColor(1.0f, 0.75f, 0.2f);
```
(Match the Category string of the neighbouring colour properties.) `RoadBuildHUD.cpp` `StyleColour`: add `case EPreviewStyle::Hover: return HoverColour;` and `case EPreviewStyle::Selected: return SelectedColour;`. In `Marker`, extend the double-ring condition to `Style == EPreviewStyle::Doomed || Style == EPreviewStyle::Pending || Style == EPreviewStyle::Selected`.

- [ ] **Step 11: Extend BuildSessionTest**

In `BuildSessionTest.cpp` after check 2 add:
```cpp
	TestEqual(TEXT("a fresh session opens in registry index 0 (Select)"), Session.GetActiveToolIndex(), 0);
```

- [ ] **Step 12: Build and test**

Build line → `Result: Succeeded`. Then:
```powershell
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool
```
Expected: the two `Airside.Tool.SelectTool.*` tests listed and `0 failed, 0 crashed`. Also run `-Filter AirportMgr` (BuildActionsTest's tool loop must still pass: every tool once with its key).

- [ ] **Step 13: Commit**

```powershell
git add -A Plugins Source
git commit -m "feat(tool): select tool as the default state

FSelection on the session, written via FToolContext::Selection; HoverAgent from the driver
picks aircraft, FindEntityAt picks stands; cancel from an idle build tool returns to Select;
switching to a build tool clears the selection. Editor gains the Select command at index 0
and its Apron/Stand labels now match the registry."
```

---

### Task 3: `ScreenPick::NearestWithin`

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/ScreenPick.h`, `Plugins/Airside/Source/Airside/Private/Tool/ScreenPick.cpp`, `Plugins/Airside/Source/AirsideTests/Private/ScreenPickTest.cpp`

**Interfaces:**
- Produces: `namespace ScreenPick { AIRSIDE_API int32 NearestWithin(TConstArrayView<FVector2D> Points, const FVector2D& Cursor, double Radius); }` returning an index into `Points` or `INDEX_NONE`.

- [ ] **Step 1: Failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/ScreenPick.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScreenPickTest,
	"Airside.Tool.ScreenPick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScreenPickTest::RunTest(const FString& Parameters)
{
	// The pixel rule the controller applies to projected agents. World-free so the one
	// judgement in aircraft picking is tested without a camera.
	const TArray<FVector2D> Points = { FVector2D(100.0, 100.0), FVector2D(110.0, 100.0), FVector2D(500.0, 500.0) };

	TestEqual(TEXT("nearest of two within radius wins"), ScreenPick::NearestWithin(Points, FVector2D(108.0, 100.0), 24.0), 1);
	TestEqual(TEXT("exactly on a point picks it"), ScreenPick::NearestWithin(Points, FVector2D(500.0, 500.0), 24.0), 2);
	TestEqual(TEXT("outside the radius of everything is none"), ScreenPick::NearestWithin(Points, FVector2D(300.0, 300.0), 24.0), INDEX_NONE);
	TestEqual(TEXT("on the radius counts as within"), ScreenPick::NearestWithin(Points, FVector2D(524.0, 500.0), 24.0), 2);
	TestEqual(TEXT("empty is none"), ScreenPick::NearestWithin(TConstArrayView<FVector2D>(), FVector2D::ZeroVector, 24.0), INDEX_NONE);
	return true;
}

#endif
```

- [ ] **Step 2: Header + source**

`Tool/ScreenPick.h`:
```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * The one judgement in picking an aircraft: which of N screen points is nearest the cursor
 * and close enough. Pulled out of the controller so it can be tested with no camera - the
 * projection is the engine's job, the rule is ours.
 */
namespace ScreenPick
{
	/** Index of the nearest point within Radius (inclusive) of Cursor, or INDEX_NONE. */
	AIRSIDE_API int32 NearestWithin(TConstArrayView<FVector2D> Points, const FVector2D& Cursor, double Radius);
}
```
`Tool/ScreenPick.cpp`:
```cpp
#include "Tool/ScreenPick.h"

namespace ScreenPick
{
	int32 NearestWithin(TConstArrayView<FVector2D> Points, const FVector2D& Cursor, double Radius)
	{
		int32 Best = INDEX_NONE;
		double BestSq = Radius * Radius;
		for (int32 I = 0; I < Points.Num(); ++I)
		{
			const double DSq = FVector2D::DistSquared(Points[I], Cursor);
			if (DSq <= BestSq)
			{
				BestSq = DSq;
				Best = I;
			}
		}
		return Best;
	}
}
```

- [ ] **Step 3: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.ScreenPick` → `1 test(s) run, 0 failed, 0 crashed`.
```powershell
git add -A Plugins
git commit -m "feat(tool): ScreenPick::NearestWithin, the pixel rule for picking aircraft"
```

---

### Task 4: Facts (`Model/InspectFacts`)

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h`, `Private/Model/InspectFacts.cpp`, `AirsideTests/Private/InspectFactsTest.cpp`
- Modify: `Model/RoadEntity.h` (`FAirframe::TypeCode`), `Entities/AircraftType.h:104-114` (`Airframe()` fills it), `Model/RoadNetwork.h/.cpp` (`FindEntityIndexByPoseNode`)

**Interfaces:**
- Produces:
  ```cpp
  struct FAgentFacts { int32 Id; FString TypeName; EAgentPhase Phase; double HeadingDegrees; double GroundSpeed; double Altitude; FString Destination; FString Status; bool bEngineRunning; bool bCanDepart; };
  struct FStandFacts { int32 Index; FString SizeClass; double DesignWingspan; int32 OccupantAgent; int32 AnchorCount; bool bReachable; };
  namespace InspectFacts {
    AIRSIDE_API bool DescribeAgent(const UGroundTraffic&, const URoadNetwork*, int32 AgentId, FAgentFacts& Out);
    AIRSIDE_API bool DescribeStand(const UGroundTraffic*, const URoadNetwork&, int32 EntityIndex, FStandFacts& Out);
    AIRSIDE_API FString IcaoCodeForWingspan(double WingspanUu);
  }
  int32 URoadNetwork::FindEntityIndexByPoseNode(FGuidelineNodeId Node) const;  // INDEX_NONE when none
  ```
- Consumes: `FRoadAgent` fields (Phase, Airframe, LastMotion, GoalNode, WaitingOn, CrossingPhase, bDepartureArmed, ShutdownCountdown, bEngineRunning, DepartureOrder), `URoadNetwork::RunwayExtentAt`, `RunwayDesignator::Designate/ToText/ToPairText`, `URoadNetwork::GetGuidelineNode`, `URoadNetwork::GetEdgesAt` (or whatever the node-degree accessor is called - grep `RoadGuideline.h`/`RoadNetwork.h` for the guideline node's edge list; `FGuidelineNode` may carry `Edges`).

- [ ] **Step 1: Failing test**

`InspectFactsTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineEdgeId InspJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectFactsTest,
	"Airside.Model.InspectFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsTest::RunTest(const FString& Parameters)
{
	// THE PANEL'S CONTRACT. The widget never reads FRoadAgent; it reads these. So each fact
	// the panel shows is pinned here, world-free, against a scripted agent - and when M3's
	// UFlight fills the same struct the panel does not change.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	InspJoin(*Net, A, B);

	// A stand whose pose node IS B, so a taxi to B is a taxi "to Stand N".
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId StandId = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 5000.0), 0.0, 1800.0);
	if (!TestTrue(TEXT("stand placed"), StandId.IsSet())) { return false; }
	// PlaceEntity makes a pose node of its own; point the fixture's route at THAT node
	// instead, so the destination lookup has something to find.
	const FGuidelineNodeId Pose = Net->GetEntity(StandId)->PoseNode;
	if (!TestTrue(TEXT("the stand has a pose node"), Pose.IsSet())) { return false; }
	InspJoin(*Net, B, Pose);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRouteQuery Q; Q.Start = A; Q.Goal = Pose; Q.Class = ETraversalClass::Aircraft;
	FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();
	Piper.TypeCode = TEXT("PA46");
	const int32 Id = Traffic->DispatchAgent(Net, RouteSearch::Find(*Net, Q), Piper, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	FAgentFacts Facts;
	TestFalse(TEXT("an unknown id yields no facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id + 99, Facts));
	if (!TestTrue(TEXT("the agent yields facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts))) { return false; }

	TestEqual(TEXT("id"), Facts.Id, Id);
	TestEqual(TEXT("type name is the airframe's code"), Facts.TypeName, FString(TEXT("PA46")));
	TestEqual(TEXT("phase"), Facts.Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("destination names the stand by index"),
		Facts.Destination, FString::Printf(TEXT("Stand %d"), StandId.Index));
	TestEqual(TEXT("status while moving freely"), Facts.Status, FString(TEXT("Taxiing")));
	TestFalse(TEXT("cannot depart while taxiing"), Facts.bCanDepart);
	TestTrue(TEXT("engine running while taxiing"), Facts.bEngineRunning);

	// Stand facts: occupied by the INBOUND agent already.
	FStandFacts SF;
	TestFalse(TEXT("a dead index yields no stand facts"), InspectFacts::DescribeStand(Traffic, *Net, 99, SF));
	if (!TestTrue(TEXT("the stand yields facts"), InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF))) { return false; }
	TestEqual(TEXT("occupant is the inbound agent"), SF.OccupantAgent, Id);
	TestEqual(TEXT("1800 uu (18 m) span is ICAO code B"), SF.SizeClass, FString(TEXT("B")));
	TestTrue(TEXT("a pose node with an edge is reachable"), SF.bReachable);
	TestEqual(TEXT("anchor count is the definition's"), SF.AnchorCount, Stand->Anchors.Num());

	// Tick to Parked: the status and bCanDepart flip, heading/speed are reported.
	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("parked"), InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts) && Facts.Phase == EAgentPhase::Parked)) { return false; }
	TestTrue(TEXT("a parked aircraft can depart"), Facts.bCanDepart);
	TestTrue(TEXT("status says shutting down while the countdown runs"), Facts.Status.StartsWith(TEXT("Shutting down")));
	TestTrue(TEXT("speed is zero when parked"), FMath::IsNearlyZero(Facts.GroundSpeed, 1.0));
	TestTrue(TEXT("heading is a compass figure"), Facts.HeadingDegrees >= 0.0 && Facts.HeadingDegrees < 360.0);

	InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF);
	TestEqual(TEXT("the parked agent still occupies the stand"), SF.OccupantAgent, Id);

	// Status precedence: WaitingOn beats everything but an armed departure.
	{
		// Scripted rather than staged with a second aircraft: the precedence is the thing
		// under test, and the arbitration that sets WaitingOn has its own tests.
		FRoadAgent Scripted = *Traffic->FindAgent(Id);
		Scripted.Phase = EAgentPhase::Taxiing;
		Scripted.WaitingOn = 7;
		TestEqual(TEXT("holding for another"), InspectFacts::StatusOf(Scripted), FString(TEXT("Holding for aircraft 7")));
		Scripted.bDepartureArmed = true;
		TestEqual(TEXT("armed departure outranks holding"), InspectFacts::StatusOf(Scripted), FString(TEXT("Departure armed")));
		Scripted.bDepartureArmed = false; Scripted.WaitingOn = 0;
		Scripted.CrossingPhase = ECrossingPhase::OnStrip;
		TestEqual(TEXT("crossing"), InspectFacts::StatusOf(Scripted), FString(TEXT("Crossing runway")));
	}

	// Empty stand: retire the agent.
	Traffic->RetireAgent(Id);
	InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF);
	TestEqual(TEXT("no occupant once the agent is gone"), SF.OccupantAgent, 0);
	return true;
}

#endif
```
Note the extra exported helper `InspectFacts::StatusOf(const FRoadAgent&)` - add it to the header (Step 3); it is what `DescribeAgent` uses.

- [ ] **Step 2: `FAirframe::TypeCode` and `Airframe()`**

`Model/RoadEntity.h`, inside `FAirframe` after `Requirements`:
```cpp
	/**
	 * The type's short code - "PA46" - for anything that has to SAY what this is. NAME_None
	 * for an airframe assembled by hand (tests, the Piper fallback). Here rather than looked
	 * up from the UAircraftType at display time because Model/ may not see Entities/, and
	 * the agent carries no pointer to its type by design (FAirframe's own comment).
	 */
	UPROPERTY(EditAnywhere) FName TypeCode;
```
`Entities/AircraftType.h` `Airframe()`: add `Out.TypeCode = Code;`.

- [ ] **Step 3: `Model/InspectFacts.h`**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"

class UGroundTraffic;
class URoadNetwork;

/**
 * What the inspector shows for an aircraft. PLAIN STRUCTS, not USTRUCTs: built every frame
 * for one panel, never saved, never Blueprint-bound - the Blueprint restyle binds to text
 * blocks the C++ widget fills, not to these.
 *
 * THE SEAM TO M3: the panel reads this and never FRoadAgent, so when UFlight exists it fills
 * the same struct (airline, scheduled off-block) and the panel does not change shape.
 */
struct FAgentFacts
{
	int32 Id = 0;
	/** FAirframe::TypeCode, or the traversal class when none ("Aircraft", "Vehicle"). */
	FString TypeName;
	EAgentPhase Phase = EAgentPhase::Gone;
	/** Compass degrees, 0 north, clockwise. */
	double HeadingDegrees = 0.0;
	/** uu per second; the panel formats. */
	double GroundSpeed = 0.0;
	/** uu above the surface. */
	double Altitude = 0.0;
	/** "Stand 3", "Runway 09", or "Node 41". */
	FString Destination;
	/** See InspectFacts::StatusOf for the precedence. */
	FString Status;
	bool bEngineRunning = false;
	/** Phase == Parked - the one precondition DepartAgent checks. */
	bool bCanDepart = false;
};

struct FStandFacts
{
	int32 Index = INDEX_NONE;
	/** ICAO code letter A-F from the design wingspan. */
	FString SizeClass;
	double DesignWingspan = 0.0;
	/** The agent whose goal is this stand's pose node and is Parked or Taxiing; 0 when none.
	 *  REPORTED, not stored: an occupancy field on the entity would be a second source of
	 *  truth traffic would have to keep in step. M3's allocator decides if it becomes one. */
	int32 OccupantAgent = 0;
	int32 AnchorCount = 0;
	/** The pose node has at least one guideline edge - an aircraft can be routed here. */
	bool bReachable = false;
};

namespace InspectFacts
{
	/** False for an unknown agent id; Out untouched. Network may be null (no destination names). */
	AIRSIDE_API bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out);

	/** False for a dead or out-of-range entity index. Traffic may be null (no occupant). */
	AIRSIDE_API bool DescribeStand(const UGroundTraffic* Traffic, const URoadNetwork& Network, int32 EntityIndex, FStandFacts& Out);

	/**
	 * One line, first match wins: Departure armed; Holding for aircraft N; Crossing runway;
	 * Shutting down (Ns); Parked; On final / Landing roll; Rolling / Climbing; Taxiing.
	 * A STRING, not an enum: presentation of several orthogonal model facts, and nothing
	 * branches on it.
	 */
	AIRSIDE_API FString StatusOf(const FRoadAgent& Agent);

	/** ICAO aerodrome reference code letter for a wingspan in uu: A <15 m, B <24, C <36, D <52, E <65, F otherwise. */
	AIRSIDE_API FString IcaoCodeForWingspan(double WingspanUu);
}
```

- [ ] **Step 4: `URoadNetwork::FindEntityIndexByPoseNode`**

`Model/RoadNetwork.h`, in the Entities section:
```cpp
	/** Index of the live entity whose PoseNode is Node, or INDEX_NONE. A linear scan: the
	 *  inspector asks once per frame for one node, and there are tens of stands. */
	int32 FindEntityIndexByPoseNode(FGuidelineNodeId Node) const;
```
`RoadNetwork.cpp`:
```cpp
int32 URoadNetwork::FindEntityIndexByPoseNode(FGuidelineNodeId Node) const
{
	if (!Node.IsSet())
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (Entities[Index].bAlive && Entities[Index].PoseNode == Node)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
```

- [ ] **Step 5: `Model/InspectFacts.cpp`**

```cpp
#include "Model/InspectFacts.h"

#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/RunwayDesignator.h"

namespace InspectFacts
{
	namespace
	{
		FString DestinationOf(const FRoadAgent& Agent, const URoadNetwork* Network)
		{
			if (Network == nullptr || !Agent.GoalNode.IsSet())
			{
				return TEXT("-");
			}
			const int32 Stand = Network->FindEntityIndexByPoseNode(Agent.GoalNode);
			if (Stand != INDEX_NONE)
			{
				return FString::Printf(TEXT("Stand %d"), Stand);
			}
			const FGuidelineNode* Node = Network->GetGuidelineNode(Agent.GoalNode);
			if (Node == nullptr)
			{
				return TEXT("-");
			}
			FVector2D Threshold, Direction;
			double Length = 0.0;
			if (Network->RunwayExtentAt(Node->Position, Threshold, Direction, Length))
			{
				// The direction it will ROLL when a departure is armed; the pair otherwise,
				// because until the planner has spoken the strip has two names.
				if (Agent.bDepartureArmed)
				{
					return FString::Printf(TEXT("Runway %s"),
						*RunwayDesignator::ToText(RunwayDesignator::Designate(Agent.DepartureOrder.Direction)));
				}
				return FString::Printf(TEXT("Runway %s"), *RunwayDesignator::ToPairText(Direction));
			}
			return FString::Printf(TEXT("Node %d"), Agent.GoalNode.Index);
		}
	}

	FString StatusOf(const FRoadAgent& Agent)
	{
		if (Agent.bDepartureArmed && Agent.Phase == EAgentPhase::Taxiing)
		{
			return TEXT("Departure armed");
		}
		if (Agent.WaitingOn != 0)
		{
			return FString::Printf(TEXT("Holding for aircraft %d"), Agent.WaitingOn);
		}
		if (Agent.CrossingPhase != ECrossingPhase::None)
		{
			return TEXT("Crossing runway");
		}
		switch (Agent.Phase)
		{
		case EAgentPhase::Parked:
			return Agent.ShutdownCountdown > 0.0
				? FString::Printf(TEXT("Shutting down (%.0fs)"), Agent.ShutdownCountdown)
				: FString(TEXT("Parked"));
		case EAgentPhase::Arriving:
			return Agent.LastMotion.bAirborne ? TEXT("On final") : TEXT("Landing roll");
		case EAgentPhase::Departing:
			return Agent.LastMotion.bAirborne ? TEXT("Climbing") : TEXT("Rolling");
		case EAgentPhase::Gone:
			return TEXT("Gone");
		case EAgentPhase::Taxiing:
		default:
			return TEXT("Taxiing");
		}
	}

	FString IcaoCodeForWingspan(double WingspanUu)
	{
		const double M = WingspanUu / 100.0;
		if (M < 15.0) { return TEXT("A"); }
		if (M < 24.0) { return TEXT("B"); }
		if (M < 36.0) { return TEXT("C"); }
		if (M < 52.0) { return TEXT("D"); }
		if (M < 65.0) { return TEXT("E"); }
		return TEXT("F");
	}

	bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
		if (Agent == nullptr)
		{
			return false;
		}
		Out.Id = Agent->Id;
		Out.TypeName = !Agent->Airframe.TypeCode.IsNone()
			? Agent->Airframe.TypeCode.ToString()
			: (Agent->Class == ETraversalClass::Aircraft ? TEXT("Aircraft") : TEXT("Vehicle"));
		Out.Phase = Agent->Phase;
		// Model heading is radians yaw from +X (east), anticlockwise. Compass is degrees from
		// north, clockwise: 90 - yaw, wrapped.
		const double Yaw = FMath::RadiansToDegrees(Agent->LastMotion.Heading);
		Out.HeadingDegrees = FMath::Fmod(FMath::Fmod(90.0 - Yaw, 360.0) + 360.0, 360.0);
		Out.GroundSpeed = Agent->LastMotion.GroundSpeed;
		Out.Altitude = Agent->LastMotion.Altitude;
		Out.Destination = DestinationOf(*Agent, Network);
		Out.Status = StatusOf(*Agent);
		Out.bEngineRunning = Agent->bEngineRunning;
		Out.bCanDepart = Agent->Phase == EAgentPhase::Parked;
		return true;
	}

	bool DescribeStand(const UGroundTraffic* Traffic, const URoadNetwork& Network, int32 EntityIndex, FStandFacts& Out)
	{
		const TArray<FEntityInstance>& Entities = Network.GetEntities();
		if (!Entities.IsValidIndex(EntityIndex) || !Entities[EntityIndex].bAlive)
		{
			return false;
		}
		const FEntityInstance& E = Entities[EntityIndex];
		Out.Index = EntityIndex;
		Out.DesignWingspan = E.DesignWingspan;
		Out.SizeClass = IcaoCodeForWingspan(E.DesignWingspan);
		Out.AnchorCount = E.ResolvedAnchors.Num();
		Out.bReachable = false;
		if (const FGuidelineNode* Pose = Network.GetGuidelineNode(E.PoseNode))
		{
			// Any live edge touching the pose node. Walk the edges rather than trust a
			// degree field: the node struct's edge list, if it has one, is derived state.
			for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
			{
				if (Edge.bAlive && (Edge.A == E.PoseNode || Edge.B == E.PoseNode))
				{
					Out.bReachable = true;
					break;
				}
			}
		}
		Out.OccupantAgent = 0;
		if (Traffic != nullptr && E.PoseNode.IsSet())
		{
			for (const FRoadAgent& Agent : Traffic->GetAgents())
			{
				if (Agent.GoalNode == E.PoseNode
					&& (Agent.Phase == EAgentPhase::Parked || Agent.Phase == EAgentPhase::Taxiing))
				{
					Out.OccupantAgent = Agent.Id;
					break;
				}
			}
		}
		return true;
	}
}
```
Verify names before building: `URoadNetwork::GetGuidelineEdges()` and `FGuidelineEdge::bAlive` (grep `RoadNetwork.h`, `RoadGuideline.h`); `FGuidelineNodeId::Index`. Adjust to the real accessor if it differs (e.g. iterate `GetGuidelineEdges()` by index with `GuidelineEdgeIdAt`).

- [ ] **Step 6: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.InspectFacts` → `1 test(s) run, 0 failed, 0 crashed`. Also `-Filter Airside.Model.RoadEntity` (the TypeCode addition must not break entity tests).
```powershell
git add -A Plugins
git commit -m "feat(model): InspectFacts - what the panel shows for an aircraft or a stand

Plain structs from FRoadAgent + URoadNetwork; status precedence and destination naming
pinned. FAirframe gains TypeCode so an agent can say what it is without seeing Entities/."
```

---

### Task 5: `DeparturePlanner::PlanAny` and `NotParked`

**Files:**
- Modify: `Model/DeparturePlanner.h`, `Private/Model/DeparturePlanner.cpp`
- Create: `AirsideTests/Private/PlanAnyTest.cpp`

**Interfaces:**
- Produces: `AIRSIDE_API FDeparturePlan DeparturePlanner::PlanAny(const URoadNetwork&, FGuidelineNodeId Start, const FAirframe&, ETraversalClass);` and `EDepartureRefusal::NotParked`.
- Consumes: `AirsideCapability::Summarise(Network).Runways` (`FRunwaySummary{Threshold, Direction, Length}`), existing `Plan`.

- [ ] **Step 1: Failing test**

`PlanAnyTest.cpp` (reuses the shape of `DeparturePlannerTest.cpp`'s fixture, redeclared with a `PlanAny` prefix against the unity build):
```cpp
#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * TWO runways: a long W-E strip at Y=0 with a 45-degree taxiway to a stand, and a short
	 * N-S strip far to the east joined by a long taxiway. The stand's nearest departure is
	 * the W-E strip; the N-S one is reachable but a longer taxi.
	 */
	struct FPlanAnyAirport
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId LongSeed;
		FRoadSegmentId ShortSeed;
		FGuidelineNodeId StandNode;
	};

	FPlanAnyAirport PlanAnyBuild(UObject* Outer)
	{
		FPlanAnyAirport Out;
		Out.Net = NewObject<URoadNetwork>(Outer);
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		const FRoadNodeId W = Out.Net->AddNode(FVector2D(-40000.0, 0.0));
		const FRoadNodeId X = Out.Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadNodeId E = Out.Net->AddNode(FVector2D(60000.0, 0.0));
		const FRoadNodeId T = Out.Net->AddNode(FVector2D(40000.0, -20000.0));
		Out.LongSeed = Out.Net->AddStraightSegment(W, X, Runway);
		Out.Net->AddStraightSegment(X, E, Runway);
		Out.Net->AddStraightSegment(X, T, Taxiway);

		// The short strip, 60000 uu east of T, joined at its midpoint.
		const FRoadNodeId S = Out.Net->AddNode(FVector2D(100000.0, -50000.0));
		const FRoadNodeId M = Out.Net->AddNode(FVector2D(100000.0, -20000.0));
		const FRoadNodeId N = Out.Net->AddNode(FVector2D(100000.0, 10000.0));
		Out.ShortSeed = Out.Net->AddStraightSegment(S, M, Runway);
		Out.Net->AddStraightSegment(M, N, Runway);
		Out.Net->AddStraightSegment(T, M, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(45000.0, -14000.0), 0.0);
		FAnchorLink::Build(*Out.Net);
		for (const FEntityInstance& I : Out.Net->GetEntities())
		{
			if (I.bAlive && I.PoseNode.IsSet()) { Out.StandNode = I.PoseNode; }
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanAnyShortestTest,
	"Airside.Model.DeparturePlanner.PlanAny.Shortest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlanAnyShortestTest::RunTest(const FString& Parameters)
{
	FPlanAnyAirport A = PlanAnyBuild(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }

	// Both strips admit the Piper; the W-E one is the shorter taxi from this stand.
	TestTrue(TEXT("chose the W-E strip (threshold on Y=0)"), FMath::Abs(Plan.Threshold.Y) < 1.0);

	// The alternative, priced: any plan to the N-S strip is longer.
	FVector2D Th, Dir; double Len = 0.0;
	FRoadSegmentId Seed;
	A.Net->RunwayExtentAt(FVector2D(100000.0, -49990.0), Th, Dir, Len, &Seed);
	const FDeparturePlan Other = DeparturePlanner::Plan(*A.Net, A.StandNode, Th + Dir * 10.0, Airframe, ETraversalClass::Aircraft);
	if (Other.IsValid())
	{
		TestTrue(TEXT("the chosen taxi is no longer than the other strip's"), Plan.Route.Length <= Other.Route.Length);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanAnyAdmissionTest,
	"Airside.Model.DeparturePlanner.PlanAny.SkipsRefusedRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlanAnyAdmissionTest::RunTest(const FString& Parameters)
{
	FPlanAnyAirport A = PlanAnyBuild(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }

	// Make the near strip grass and demand tarmac: PlanAny must go to the far one.
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	A.Net->SetRunwayFacts(A.LongSeed, Grass);
	FRunwayFacts Tarmac;
	Tarmac.Surface = ERunwaySurface::Tarmac;
	A.Net->SetRunwayFacts(A.ShortSeed, Tarmac);
	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Airframe.Requirements.MinimumSurface = ERunwaySurface::Tarmac;

	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }
	TestTrue(TEXT("chose the N-S strip (threshold on X=100000)"), FMath::Abs(Plan.Threshold.X - 100000.0) < 1.0);

	// Both refused: the refusal names admission, not "no runway".
	A.Net->SetRunwayFacts(A.ShortSeed, Grass);
	const FDeparturePlan None = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	TestFalse(TEXT("no plan when every strip refuses"), None.IsValid());
	TestEqual(TEXT("the reason is the admission, so the log says why"), None.Why, EDepartureRefusal::NotAdmitted);

	// An empty network: NoRunway.
	URoadNetwork* Empty = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId Lone = Empty->AddGuidelineNode(FVector2D::ZeroVector, false);
	TestEqual(TEXT("no runways at all is NoRunway"),
		DeparturePlanner::PlanAny(*Empty, Lone, Airframe, ETraversalClass::Aircraft).Why, EDepartureRefusal::NoRunway);
	return true;
}

#endif
```
Check `FRunwayFacts`/`ERunwaySurface`/`Requirements.MinimumSurface` names against `Model/RunwayFacts.h` and `DeparturePlannerTest.cpp:234-260` (they are used there).

- [ ] **Step 2: Header**

`Model/DeparturePlanner.h`: add to the enum after `NotAdmitted`:
```cpp
	/** DepartAgent only: the agent is not Parked, so there is nothing standing still to send. */
	NotParked,
```
Add to the namespace:
```cpp
	/**
	 * Plan a departure from Start onto WHICHEVER runway gives the shortest admitted taxi.
	 * Both thresholds of every chain are tried through Plan. When none is valid the first
	 * refusal is returned, so the log can say "grass strip, needs tarmac" rather than
	 * "no runway". The inspector's Depart button; M3's sequencer replaces the choice, not
	 * the shape.
	 */
	AIRSIDE_API FDeparturePlan PlanAny(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FAirframe& Airframe, ETraversalClass Class);
```

- [ ] **Step 3: Implementation**

`DeparturePlanner.cpp`: add `#include "Model/AirsideCapability.h"`; before `Describe`:
```cpp
	FDeparturePlan PlanAny(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FAirframe& Airframe, ETraversalClass Class)
	{
		// The capability summary already enumerates chains once each, by threshold pair;
		// re-deriving that walk here would be a second enumerator to keep in step.
		const FAirsideCapability Cap = AirsideCapability::Summarise(Network);

		FDeparturePlan Best;
		Best.Why = EDepartureRefusal::NoRunway;
		bool bHaveRefusal = false;

		for (const FRunwaySummary& R : Cap.Runways)
		{
			// A point just inside EACH end: RunwayExtentAt's proximity gate is against the
			// nearest segment end, so a midpoint on a long segment is "not on a runway" and
			// the threshold it hands back is the one nearest the point asked about.
			const FVector2D Ends[2] = { R.Threshold + R.Direction * 10.0, R.Threshold + R.Direction * (R.Length - 10.0) };
			for (const FVector2D& OnRunway : Ends)
			{
				const FDeparturePlan Candidate = Plan(Network, Start, OnRunway, Airframe, Class);
				if (Candidate.IsValid())
				{
					if (!Best.IsValid() || Candidate.Route.Length < Best.Route.Length)
					{
						Best = Candidate;
					}
				}
				else if (!Best.IsValid() && !bHaveRefusal)
				{
					Best = Candidate;
					bHaveRefusal = true;
				}
			}
		}
		return Best;
	}
```
In `Describe`, add `case EDepartureRefusal::NotParked: return TEXT("Departure refused: the aircraft is not parked.");`.

- [ ] **Step 4: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.DeparturePlanner` → 6 tests (4 existing + 2 new), `0 failed, 0 crashed`. If `PlanAny.Shortest` fails because the taxiway geometry does not route, log `Describe` for each `Plan` call in the test and adjust the fixture coordinates (the T-to-M taxiway must meet the short strip at M; a 45-degree join is not required there).
```powershell
git add -A Plugins
git commit -m "feat(model): DeparturePlanner::PlanAny - shortest admitted taxi over every runway"
```

---

### Task 6: `DepartAgent` through the layers

**Files:**
- Modify: `Model/GroundTraffic.h/.cpp`, `Present/AirsideTraffic.h/.cpp`, `Present/RoadNetworkActor.h/.cpp`
- Create: `AirsideTests/Private/DepartAgentTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  EDepartureRefusal UGroundTraffic::DepartAgent(int32 AgentId, const URoadNetwork& Network);
  EDepartureRefusal UAirsideTraffic::DepartAgent(int32 AgentId, const URoadNetwork* Network);
  ARoadAgentActor* UAirsideTraffic::GetAgentView(int32 AgentId) const;
  EDepartureRefusal ARoadNetworkActor::DepartAgent(int32 AgentId);
  ARoadAgentActor* ARoadNetworkActor::GetAgentView(int32 AgentId) const;
  ```

- [ ] **Step 1: Failing test**

`DepartAgentTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The departure-release fixture: a runway split at (0,0) and an authored guideline from
	 *  A (0,-20000) to B (0,0) ON the strip. Built onto whichever network is handed in. */
	struct FDepAgentGraph { FGuidelineNodeId A, B; };

	FDepAgentGraph DepAgentBuild(URoadNetwork& Net)
	{
		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		FDepAgentGraph G;
		G.A = Net.AddGuidelineNode(FVector2D(0.0, -20000.0), false);
		G.B = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = G.A; Edge.B = G.B;
		Edge.Control = FVector2D(0.0, -10000.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return G;
	}

	FAirframe DepAgentPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartAgentModelTest,
	"Airside.Model.Traffic.DepartAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartAgentModelTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FDepAgentGraph G = DepAgentBuild(*Net);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// Taxi B -> A: parks at A, off the runway, engine shut down after the pause.
	FRouteQuery Q; Q.Start = G.B; Q.Goal = G.A; Q.Class = ETraversalClass::Aircraft;
	const int32 Id = Traffic->DispatchAgent(Net, RouteSearch::Find(*Net, Q), DepAgentPiper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	TestEqual(TEXT("a taxiing agent may not depart"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::NotParked);
	TestEqual(TEXT("an unknown id is NotParked too - there is nothing parked by that id"), Traffic->DepartAgent(Id + 9, *Net), EDepartureRefusal::NotParked);

	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic->Advance(1.0 / 30.0, Net); }
	if (!TestEqual(TEXT("parked"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked)) { return false; }
	for (int32 I = 0; I < 300 && Traffic->FindAgent(Id)->bEngineRunning; ++I) { Traffic->Advance(0.1, Net); }
	if (!TestFalse(TEXT("engine shut down after the pause"), Traffic->FindAgent(Id)->bEngineRunning)) { return false; }

	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("a parked agent departs (%d)"), static_cast<int32>(Why)), Why, EDepartureRefusal::None)) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Id);
	TestEqual(TEXT("it is taxiing again"), P->Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("with a departure armed"), P->bDepartureArmed);
	TestTrue(TEXT("and the engine running - a redirect restarts it"), P->bEngineRunning);
	TestEqual(TEXT("departing twice is refused: it is no longer parked"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::NotParked);

	bool bDeparted = false;
	for (double T = 0.0; T < 300.0; T += 0.05)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* Now = Traffic->FindAgent(Id);
		if (Now == nullptr) { break; }
		bDeparted = bDeparted || Now->Phase == EAgentPhase::Departing;
	}
	TestTrue(TEXT("it rolled"), bDeparted);
	TestNull(TEXT("and it is gone"), Traffic->FindAgent(Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartAgentForwardersTest,
	"Airside.Present.DepartAgentForwarders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartAgentForwardersTest::RunTest(const FString& Parameters)
{
	// THE SEAM TEST: the panel calls the actor; the actor must reach the model, and the
	// view must follow the agent out. A forwarder that was never wired compiles fine.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	const FDepAgentGraph G = DepAgentBuild(*Actor->Network);

	FRouteQuery Q; Q.Start = G.B; Q.Goal = G.A; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched through the actor"), Actor->DispatchAgent(RouteSearch::Find(*Actor->Network, Q), DepAgentPiper()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	TestNotNull(TEXT("the actor can name the agent's view by id"), Actor->GetAgentView(Id));
	TestNull(TEXT("and returns null for an unknown id"), Actor->GetAgentView(Id + 9));

	TestEqual(TEXT("refused while taxiing, through the actor"), Actor->DepartAgent(Id), EDepartureRefusal::NotParked);
	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	TestEqual(TEXT("accepted once parked, through the actor"), Actor->DepartAgent(Id), EDepartureRefusal::None);

	for (int32 I = 0; I < 20000 && Actor->GetAgentCount() > 0; ++I) { Actor->Tick(1.0f / 30.0f); }
	TestEqual(TEXT("the agent departed and was dropped"), Actor->GetAgentCount(), 0);
	TestNull(TEXT("and its view went with it"), Actor->GetAgentView(Id));
	return true;
}

#endif
```

- [ ] **Step 2: `UGroundTraffic::DepartAgent`**

`Model/GroundTraffic.h`: add `#include "Model/DeparturePlanner.h"` (or forward-declare the enum: `enum class EDepartureRefusal : uint8;` - prefer the forward declaration and include in the .cpp). After `RedirectAgent`:
```cpp
	/**
	 * Sends a PARKED agent to whichever runway gives the shortest admitted taxi, with the
	 * take-off armed - the inspector's Depart button. Anything not Parked is refused as
	 * NotParked: a taxiing aircraft has a plan, an arriving one is not on the ground, a
	 * departing one is already going. Composed from PlanAny and RedirectAgent so there is
	 * one arming path (ArmDepartureIfRunway) and one engine restart (StartTaxi).
	 */
	EDepartureRefusal DepartAgent(int32 AgentId, const URoadNetwork& Network);
```
`GroundTraffic.cpp`, after `RedirectAgent`:
```cpp
EDepartureRefusal UGroundTraffic::DepartAgent(int32 AgentId, const URoadNetwork& Network)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Parked)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: %s"), AgentId,
			Index == INDEX_NONE ? TEXT("no such agent") : *UEnum::GetValueAsString(Agents[Index].Phase));
		return EDepartureRefusal::NotParked;
	}
	const FRoadAgent& Agent = Agents[Index];
	if (!Agent.GoalNode.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: parked at no node."), AgentId);
		return EDepartureRefusal::NoRoute;
	}

	// From where it PARKED - its goal node - not from its polyline position: the search is
	// over the graph and the pose node is the graph's name for this stand.
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(Network, Agent.GoalNode, Agent.Airframe, Agent.Class);
	UE_LOG(LogAirsideTraffic, Log, TEXT("DepartAgent %d: %s"), AgentId, *DeparturePlanner::Describe(Plan));
	if (!Plan.IsValid())
	{
		return Plan.Why;
	}
	if (!RedirectAgent(AgentId, &Network, Plan.Route))
	{
		return EDepartureRefusal::NoRoute;
	}
	return EDepartureRefusal::None;
}
```
Add `#include "Model/DeparturePlanner.h"` to `GroundTraffic.cpp`.

- [ ] **Step 3: `UAirsideTraffic` forwarders**

`Present/AirsideTraffic.h`: forward-declare `enum class EDepartureRefusal : uint8;`; after `RedirectAgent`:
```cpp
	/** See UGroundTraffic::DepartAgent. A null Network is refused as NoRoute. */
	EDepartureRefusal DepartAgent(int32 AgentId, const URoadNetwork* Network);

	/** The actor showing agent AgentId, or null. For the follow camera and the picker. */
	ARoadAgentActor* GetAgentView(int32 AgentId) const;
```
`AirsideTraffic.cpp`:
```cpp
EDepartureRefusal UAirsideTraffic::DepartAgent(int32 AgentId, const URoadNetwork* Network)
{
	if (Network == nullptr)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d: no network to plan over."), AgentId);
		return EDepartureRefusal::NoRoute;
	}
	return Model->DepartAgent(AgentId, *Network);
}

ARoadAgentActor* UAirsideTraffic::GetAgentView(int32 AgentId) const
{
	const TObjectPtr<ARoadAgentActor>* Found = Views.Find(AgentId);
	return Found != nullptr ? Found->Get() : nullptr;
}
```
Add `#include "Model/DeparturePlanner.h"` to the .cpp.

- [ ] **Step 4: Actor forwarders**

`Present/RoadNetworkActor.h`, in the Agents section:
```cpp
	/** Sends a parked agent to the runway and arms its take-off. Forwards to Traffic. */
	EDepartureRefusal DepartAgent(int32 AgentId);

	/** The actor showing an agent, or null. Forwards to Traffic. */
	ARoadAgentActor* GetAgentView(int32 AgentId) const;
```
plus `enum class EDepartureRefusal : uint8;` among the forward declarations. `RoadNetworkActor.cpp`:
```cpp
EDepartureRefusal ARoadNetworkActor::DepartAgent(int32 AgentId)
{
	return Traffic->DepartAgent(AgentId, Network);
}

ARoadAgentActor* ARoadNetworkActor::GetAgentView(int32 AgentId) const
{
	return Traffic->GetAgentView(AgentId);
}
```

- [ ] **Step 5: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Traffic.DepartAgent` and `-Filter Airside.Present.DepartAgentForwarders` → each `1 test(s) run, 0 failed, 0 crashed`. If the model test's "it rolled" fails, log `Describe(Plan)` from the test (the fixture is the departure-release one, which arms today; a change in `PlanAny`'s end points is the likely cause).
```powershell
git add -A Plugins
git commit -m "feat(traffic): DepartAgent - a parked aircraft plans to the nearest admitted runway and goes

UGroundTraffic::DepartAgent = PlanAny + RedirectAgent; NotParked for anything else.
Forwarded through UAirsideTraffic and the actor with GetAgentView beside it."
```

---

### Task 7: Controller: hover pick, selection, Depart and Follow actions

**Files:**
- Modify: `Source/AirportMgr/RoadBuildController.h/.cpp`, `Source/AirportMgr/BuildActions.h/.cpp`, `Source/AirportMgr/BuildActionsTest.cpp`, `Source/AirportMgr/BuildBarWidgetTest.cpp`, `Source/AirportMgr/BuildBarWidget.h/.cpp` (new section slot)

**Interfaces:**
- Produces on `ARoadBuildController`:
  ```cpp
  const FSelection& GetSelection() const;
  bool HasSelectedAircraft() const;
  bool CanDepartSelected() const;     // facts.bCanDepart for the selected aircraft
  void DepartSelected();
  bool SelectedAgentFacts(FAgentFacts& Out) const;
  bool SelectedStandFacts(FStandFacts& Out) const;
  UPROPERTY(EditAnywhere) double AgentPickPixels = 24.0;
  ```
  `EActionSection::Selection` between `Aircraft` and `Game`; actions `selection.depart` (no key) and `selection.follow` (C), the latter replacing `aircraft.watch`.

- [ ] **Step 1: Failing test edits**

`BuildActionsTest.cpp`: the section loop `for (uint8 S = 0; S <= static_cast<uint8>(EActionSection::Game); ++S)` already covers a new enum value if `Game` stays last. Add after the tool loop:
```cpp
	// The inspector's verbs are rows of THIS table, so the panel's buttons and the C key
	// cannot diverge. Depart is bar/panel only (no key); Follow took the old Watch key.
	TestTrue(TEXT("selection.depart is registered"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("selection.depart")) && A.Section == EActionSection::Selection && !A.Key.IsValid(); }));
	TestTrue(TEXT("selection.follow is registered on C"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("selection.follow")) && A.Key == EKeys::C; }));
	TestFalse(TEXT("aircraft.watch is gone - one verb for following"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("aircraft.watch")); }));
```
`BuildBarWidgetTest.cpp`: the loop is generic; no change needed beyond `ActionSectionName` handling the new value (Step 3). Run `-Filter AirportMgr.Actions` first to see the registry test fail on the three new assertions.

- [ ] **Step 2: Controller header**

`RoadBuildController.h`: add `#include "Tool/Selection.h"` and forward declare `struct FAgentFacts; struct FStandFacts; class UInspectorWidget;`. Near `ToolPickRadius`:
```cpp
	/**
	 * How close, in PIXELS, the cursor must be to an aircraft's projected position to pick
	 * it. Pixels, not uu: an aircraft on final is clicked in screen space (spec §3.3), and a
	 * radius that shrank with distance would make the far ones unclickable.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Snap", meta = (ClampMin = "1.0"))
	double AgentPickPixels = 24.0;
```
In the Actions block:
```cpp
	// --- Selection (the inspector's verbs) --------------------------------------------
	const FSelection& GetSelection() const { return Session.GetSelection(); }
	bool HasSelectedAircraft() const { return GetSelection().Kind == ESelectionKind::Aircraft; }
	/** The selected aircraft's facts, or false when nothing is selected or it has gone. */
	bool SelectedAgentFacts(FAgentFacts& Out) const;
	bool SelectedStandFacts(FStandFacts& Out) const;
	bool CanDepartSelected() const;
	/** Depart the selected aircraft; logs the planner's answer. */
	void DepartSelected();
```
Private:
```cpp
	/** The agent whose projected position is nearest the cursor within AgentPickPixels, or 0. */
	int32 HoverAgentUnderCursor() const;

	/** The agent the watch camera rides: the selected one at toggle time, else the newest. */
	int32 WatchAgentId = 0;
```
Update `ToggleWatchAgent`'s doc: "C: orbit the SELECTED aircraft, or the newest when none is selected; or go back to the build view."

- [ ] **Step 3: Controller source**

`RoadBuildController.cpp`: add `#include "Model/InspectFacts.h"`, `#include "Model/GroundTraffic.h"`, `#include "Present/AirsideTraffic.h"`, `#include "Tool/ScreenPick.h"`.

Replace `MakeToolContext`'s final `return Session.MakeContext(...)` to pass `HoverAgentUnderCursor()` as the last argument.

Add:
```cpp
int32 ARoadBuildController::HoverAgentUnderCursor() const
{
	if (Target == nullptr || Target->GetTraffic() == nullptr || Target->GetTraffic()->GetModel() == nullptr)
	{
		return 0;
	}
	float MouseX = 0.0f, MouseY = 0.0f;
	if (!GetMousePosition(MouseX, MouseY))
	{
		return 0;
	}
	// Project the VIEW's location, not the model's road-plane position: the view carries
	// altitude, and an aircraft on final is picked where it is drawn.
	TArray<FVector2D> Screen;
	TArray<int32> Ids;
	for (const FRoadAgent& Agent : Target->GetTraffic()->GetModel()->GetAgents())
	{
		const ARoadAgentActor* View = Target->GetAgentView(Agent.Id);
		FVector2D At;
		if (View != nullptr && ProjectWorldLocationToScreen(View->GetActorLocation(), At))
		{
			Screen.Add(At);
			Ids.Add(Agent.Id);
		}
	}
	const int32 Pick = ScreenPick::NearestWithin(Screen, FVector2D(MouseX, MouseY), AgentPickPixels);
	return Pick != INDEX_NONE ? Ids[Pick] : 0;
}

bool ARoadBuildController::SelectedAgentFacts(FAgentFacts& Out) const
{
	const FSelection& Sel = GetSelection();
	if (Sel.Kind != ESelectionKind::Aircraft || Target == nullptr || Target->GetGroundTraffic() == nullptr)
	{
		return false;
	}
	return InspectFacts::DescribeAgent(*Target->GetGroundTraffic(), Target->GetNetwork(), Sel.Id, Out);
}

bool ARoadBuildController::SelectedStandFacts(FStandFacts& Out) const
{
	const FSelection& Sel = GetSelection();
	if (Sel.Kind != ESelectionKind::Stand || Target == nullptr || Target->GetNetwork() == nullptr)
	{
		return false;
	}
	return InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Sel.Id, Out);
}

bool ARoadBuildController::CanDepartSelected() const
{
	FAgentFacts Facts;
	return SelectedAgentFacts(Facts) && Facts.bCanDepart;
}

void ARoadBuildController::DepartSelected()
{
	if (!HasSelectedAircraft() || Target == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Depart: no aircraft selected."));
		return;
	}
	const EDepartureRefusal Why = Target->DepartAgent(GetSelection().Id);
	UE_LOG(LogRoadBuild, Log, TEXT("Depart aircraft %d: %s"), GetSelection().Id,
		Why == EDepartureRefusal::None ? TEXT("accepted") : *UEnum::GetValueAsString(Why));
}
```
Add `#include "Model/DeparturePlanner.h"` for the enum. `UEnum::GetValueAsString` needs the enum to be a `UENUM` - `EDepartureRefusal` is (`UENUM()` in DeparturePlanner.h).

`ToggleWatchAgent`: replace the `GetNewestAgent() == nullptr` guard and entry with:
```cpp
	const int32 Wanted = HasSelectedAircraft() ? GetSelection().Id : Target->GetTraffic()->GetNewestAgentId();
	if (!bWatchingAgent && Target->GetAgentView(Wanted) == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning,
			TEXT("Nothing to follow: select an aircraft, or land one (7) first."));
		return;
	}

	bWatchingAgent = !bWatchingAgent;
	if (bWatchingAgent)
	{
		WatchAgentId = Wanted;
		...existing reset of WatchTarget...
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Camera: %s"),
		bWatchingAgent ? *FString::Printf(TEXT("following aircraft %d"), WatchAgentId) : TEXT("build view"));
```
In `PlayerTick`, replace `if (ARoadAgentActor* Agent = Target->GetNewestAgent())` with `if (ARoadAgentActor* Agent = Target->GetAgentView(WatchAgentId))`.

- [ ] **Step 4: Actions**

`BuildActions.h`: insert `Selection,` between `Aircraft` and `Game` in `EActionSection`. `BuildActions.cpp`: `ActionSectionName` gains `case EActionSection::Selection: return TEXT("Selection");`. Delete the `aircraft.watch` row. After the Aircraft section:
```cpp
		// --- Selection: the inspector's verbs. Rows HERE so the panel's buttons, the bar
		// and the C key are one list (spec §6.2). Depart has no key: a key that departed
		// whatever happened to be selected is a misclick away from an unintended take-off.
		Out.Add(Make(TEXT("selection.depart"), EActionSection::Selection, LOCTEXT("Depart", "Depart"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.DepartSelected(); }, Never,
			[](const ARoadBuildController& C) { return C.CanDepartSelected(); }));
		Out.Add(Make(TEXT("selection.follow"), EActionSection::Selection, LOCTEXT("Follow", "Follow"), EKeys::C, false,
			[](ARoadBuildController& C) { C.ToggleWatchAgent(); },
			[](const ARoadBuildController& C) { return C.IsWatchingAgent(); },
			[](const ARoadBuildController& C) { return C.HasSelectedAircraft() || C.HasAgent() || C.IsWatchingAgent(); }));
```

- [ ] **Step 5: Bar section slot**

`BuildBarWidget.h`: add `UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> SelectionSection;` after `AircraftSection`; update the class doc's list of panel names to include `SelectionSection`. `.cpp`: `Ensure(SelectionSection, TEXT("SelectionSection"));` after the Aircraft one; `SectionPanel` gains `case EActionSection::Selection: return SelectionSection;`.

- [ ] **Step 6: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr` → all `AirportMgr.*` tests, `0 failed, 0 crashed`.
```powershell
git add -A Source
git commit -m "feat(game): selection reaches the controller - hover pick, Depart and Follow actions

Hover agent projected per frame and handed to the session; Depart (bar only) and Follow (C,
replacing Watch) join the one action table in a Selection section; Follow rides the
selected aircraft, newest when none."
```

---

### Task 8: `UInspectorWidget`

**Files:**
- Create: `Source/AirportMgr/InspectorWidget.h`, `Source/AirportMgr/InspectorWidget.cpp`, `Source/AirportMgr/InspectorWidgetTest.cpp`
- Modify: `Source/AirportMgr/RoadBuildController.h/.cpp` (create it beside the bar)

**Interfaces:**
- Produces:
  ```cpp
  UCLASS() class AIRPORTMGR_API UInspectorWidget : public UUserWidget {
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText, FactsText, StatusText;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> DepartButton, FollowButton;
    void Refresh(const ARoadNetworkActor* Target, const FSelection& Selection);  // what NativeTick calls
    bool IsDepartEnabledForTest() const; FString TitleForTest() const; bool IsShownForTest() const;
  };
  ```

- [ ] **Step 1: Failing test**

`InspectorWidgetTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/Selection.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorWidgetTest,
	"AirportMgr.Inspector.ReadsFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorWidgetTest::RunTest(const FString& Parameters)
{
	// THE PANEL IS WIRED. A real widget, a real actor, a real agent: Depart greys while it
	// taxis and lights when it parks; the title names the aircraft; no selection hides it.
	// Refresh is called directly with the same arguments NativeTick passes, because a
	// headless test has no controller to poll through - the tick-to-Refresh seam is one
	// line and is read, not run, here.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(RouteSearch::Find(Net, Q), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }

	FSelection None;
	Panel->Refresh(Actor, None);
	TestFalse(TEXT("hidden with nothing selected"), Panel->IsShownForTest());

	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("shown with an aircraft selected"), Panel->IsShownForTest());
	TestTrue(TEXT("the title names the aircraft"), Panel->TitleForTest().Contains(FString::FromInt(Id)));
	TestFalse(TEXT("Depart is greyed while taxiing"), Panel->IsDepartEnabledForTest());

	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("Depart lights once parked"), Panel->IsDepartEnabledForTest());
	return true;
}

#endif
```

- [ ] **Step 2: Header**

`InspectorWidget.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Tool/Selection.h"
#include "InspectorWidget.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UButton;
class UTextBlock;

/**
 * The inspector: what the selected aircraft or stand is doing, and the verbs for it.
 *
 * The same recipe as UBuildBarWidget: a C++ base that builds a working panel with no asset,
 * and BindWidgetOptional slots a Widget Blueprint fills to restyle it. Set the Blueprint
 * as InspectorClass on the controller.
 *
 * POLLED each tick from InspectFacts, never subscribed: the bar's enabled states were
 * event-driven once and went stale, and a panel that shows speed needs every frame anyway.
 * It reads FAgentFacts / FStandFacts and never FRoadAgent, so M3's UFlight fills the same
 * struct and this file does not change.
 *
 * Its buttons run rows of BuildActions() by id, so the panel, the bar and the C key are one
 * list (spec §6.2).
 */
UCLASS()
class AIRPORTMGR_API UInspectorWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FactsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> DepartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> FollowButton;

	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor PanelTint = FLinearColor(0.06f, 0.07f, 0.09f, 0.92f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor ButtonTint = FLinearColor(0.18f, 0.20f, 0.24f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor DisabledTint = FLinearColor(0.10f, 0.10f, 0.12f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") int32 FontSize = 12;
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double PanelWidth = 300.0;
	/** Distance above the bottom edge, so it clears the build bar. */
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double BottomOffset = 72.0;

	/**
	 * Re-reads the facts for Selection over Target and repaints. What NativeTick calls with
	 * the controller's target and selection; public so a headless test can drive it with
	 * no controller.
	 */
	void Refresh(const ARoadNetworkActor* Target, const FSelection& Selection);

	virtual bool Initialize() override;

	bool IsShownForTest() const;
	bool IsDepartEnabledForTest() const;
	FString TitleForTest() const;

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	bool bBuilt = false;
	bool bDepartEnabled = false;

	ARoadBuildController* Controller() const;
	void EnsureSlots();
	void RunActionById(FName Id);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
};
```

- [ ] **Step 3: Source**

`InspectorWidget.cpp`:
```cpp
#include "InspectorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"

DEFINE_LOG_CATEGORY_STATIC(LogInspector, Log, All);

ARoadBuildController* UInspectorWidget::Controller() const
{
	if (APlayerController* Owning = GetOwningPlayer())
	{
		return Cast<ARoadBuildController>(Owning);
	}
	return GetWorld() ? Cast<ARoadBuildController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

bool UInspectorWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;
	EnsureSlots();
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	SetVisibility(ESlateVisibility::Collapsed);
	return bOk;
}

void UInspectorWidget::EnsureSlots()
{
	// Code-built chrome only where the asset gave none - the same rule as the bar. A
	// bottom-left card: title, facts, status, then the two verbs in a row.
	UVerticalBox* Column = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("InspectorRoot"));
		WidgetTree->RootWidget = Root;
		UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("InspectorCard"));
		Card->SetBrushColor(PanelTint);
		Card->SetPadding(FMargin(12.0f, 10.0f));
		UCanvasPanelSlot* CardSlot = Root->AddChildToCanvas(Card);
		CardSlot->SetAnchors(FAnchors(0.0f, 1.0f, 0.0f, 1.0f));
		CardSlot->SetAlignment(FVector2D(0.0, 1.0));
		CardSlot->SetAutoSize(true);
		CardSlot->SetPosition(FVector2D(12.0, -BottomOffset));
		Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InspectorColumn"));
		Card->SetContent(Column);
		UE_LOG(LogInspector, Log, TEXT("No inspector asset: building the code-only panel"));
	}

	auto Text = [&](TObjectPtr<UTextBlock>& Slot, const TCHAR* Name)
	{
		if (Slot != nullptr) { return; }
		Slot = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		FSlateFontInfo Font = Slot->GetFont();
		Font.Size = FontSize;
		Slot->SetFont(Font);
		Slot->SetAutoWrapText(true);
		Slot->SetMinDesiredWidth(static_cast<float>(PanelWidth));
		if (Column != nullptr) { Column->AddChildToVerticalBox(Slot)->SetPadding(FMargin(0.0f, 2.0f)); }
	};
	Text(TitleText, TEXT("TitleText"));
	Text(FactsText, TEXT("FactsText"));
	Text(StatusText, TEXT("StatusText"));

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr))
	{
		Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InspectorVerbs"));
		Column->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}
	auto Button = [&](TObjectPtr<UButton>& Slot, const TCHAR* Name, const TCHAR* Caption)
	{
		if (Slot != nullptr) { return; }
		Slot = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetText(FText::FromString(Caption));
		FSlateFontInfo Font = Label->GetFont();
		Font.Size = FontSize;
		Label->SetFont(Font);
		Slot->SetContent(Label);
		Slot->SetBackgroundColor(ButtonTint);
		if (Row != nullptr) { Row->AddChildToHorizontalBox(Slot)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	};
	Button(DepartButton, TEXT("DepartButton"), TEXT("Depart"));
	Button(FollowButton, TEXT("FollowButton"), TEXT("Follow (C)"));
}

void UInspectorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (const ARoadBuildController* C = Controller())
	{
		Refresh(C->GetTarget(), C->GetSelection());
	}
}

void UInspectorWidget::Refresh(const ARoadNetworkActor* Target, const FSelection& Selection)
{
	if (Target == nullptr || !Selection.IsSet())
	{
		SetVisibility(ESlateVisibility::Collapsed);
		bDepartEnabled = false;
		return;
	}

	FString Title, Facts, Status;
	bool bAircraft = false;
	if (Selection.Kind == ESelectionKind::Aircraft)
	{
		FAgentFacts F;
		if (Target->GetGroundTraffic() == nullptr || !InspectFacts::DescribeAgent(*Target->GetGroundTraffic(), Target->GetNetwork(), Selection.Id, F))
		{
			SetVisibility(ESlateVisibility::Collapsed);
			bDepartEnabled = false;
			return;
		}
		bAircraft = true;
		Title = FString::Printf(TEXT("%s  #%d"), *F.TypeName, F.Id);
		// m/s and knots side by side: the sim's unit and the one a pilot reads.
		Facts = FString::Printf(TEXT("Heading %03.0f\nSpeed %.1f m/s (%.0f kt)\nAltitude %.0f m\nTo %s\nEngine %s"),
			F.HeadingDegrees, F.GroundSpeed / 100.0, F.GroundSpeed / 100.0 * 1.94384, F.Altitude / 100.0,
			*F.Destination, F.bEngineRunning ? TEXT("running") : TEXT("off"));
		Status = F.Status;
		bDepartEnabled = F.bCanDepart;
	}
	else
	{
		FStandFacts S;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Selection.Id, S))
		{
			SetVisibility(ESlateVisibility::Collapsed);
			bDepartEnabled = false;
			return;
		}
		Title = FString::Printf(TEXT("Stand %d"), S.Index);
		Facts = FString::Printf(TEXT("Code %s (%.0f m span)\n%d service anchors\n%s"),
			*S.SizeClass, S.DesignWingspan / 100.0, S.AnchorCount,
			S.bReachable ? TEXT("Reachable by taxiway") : TEXT("NOT reachable - no taxiway joins it"));
		Status = S.OccupantAgent != 0 ? FString::Printf(TEXT("Occupied by aircraft #%d"), S.OccupantAgent) : FString(TEXT("Empty"));
		bDepartEnabled = false;
	}

	if (TitleText != nullptr) { TitleText->SetText(FText::FromString(Title)); }
	if (FactsText != nullptr) { FactsText->SetText(FText::FromString(Facts)); }
	if (StatusText != nullptr) { StatusText->SetText(FText::FromString(Status)); }
	if (DepartButton != nullptr)
	{
		DepartButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		DepartButton->SetIsEnabled(bDepartEnabled);
		DepartButton->SetBackgroundColor(bDepartEnabled ? ButtonTint : DisabledTint);
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UInspectorWidget::RunActionById(FName Id)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Inspector %s ignored: no controller"), *Id.ToString());
		return;
	}
	for (const FBuildAction& A : BuildActions())
	{
		if (A.Id == Id)
		{
			if (A.IsEnabled(*C))
			{
				UE_LOG(LogInspector, Log, TEXT("Inspector: %s"), *Id.ToString());
				A.Execute(*C);
			}
			return;
		}
	}
	UE_LOG(LogInspector, Warning, TEXT("Inspector: no action named %s"), *Id.ToString());
}

void UInspectorWidget::HandleDepart() { RunActionById(TEXT("selection.depart")); }
void UInspectorWidget::HandleFollow() { RunActionById(TEXT("selection.follow")); }

bool UInspectorWidget::IsShownForTest() const { return GetVisibility() != ESlateVisibility::Collapsed; }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
```
Note `SetVisibility(SelfHitTestInvisible)` on the root so the card's buttons take clicks but the empty canvas does not swallow them from the tool underneath.

- [ ] **Step 4: Controller creates it**

`RoadBuildController.h` next to `BuildBarClass`/`BuildBar`:
```cpp
	/** The inspector's Blueprint class; null means the plain C++ panel. Config, like the bar's. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UInspectorWidget> InspectorClass;

	UPROPERTY(Transient) TObjectPtr<UInspectorWidget> Inspector;
```
`RoadBuildController.cpp` `BeginPlay`, after the bar is added:
```cpp
	const TSubclassOf<UInspectorWidget> PanelClass =
		InspectorClass != nullptr ? InspectorClass : TSubclassOf<UInspectorWidget>(UInspectorWidget::StaticClass());
	Inspector = CreateWidget<UInspectorWidget>(this, PanelClass);
	if (Inspector != nullptr)
	{
		Inspector->AddToViewport(1);
		UE_LOG(LogRoadBuild, Log, TEXT("Inspector: %s"),
			InspectorClass != nullptr ? *InspectorClass->GetName() : TEXT("code-only (no InspectorClass configured)"));
	}
```
Add `#include "InspectorWidget.h"`. Update the banner text: replace `Left click places and connects, right click ends the chain.` with `Click an aircraft or stand to inspect it; pick a tool to build; right click puts a tool down.`

- [ ] **Step 5: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr` → includes `AirportMgr.Inspector.ReadsFacts`, `0 failed, 0 crashed`.
```powershell
git add -A Source
git commit -m "feat(ui): inspector panel - facts for the selected aircraft or stand, Depart and Follow

UMG C++ base with BindWidgetOptional slots for a Blueprint restyle; polls InspectFacts each
tick; buttons run BuildActions rows by id."
```

---

### Task 9: Full run, lint, log-count, PR

**Files:** none new. `docs/superpowers/specs/2026-09-07-entity-inspector-design.md` §7 if anything else deviated.

- [ ] **Step 1: Architecture lint and full test run**

```powershell
./Tools/Check-Architecture.ps1
./Tools/Run-AirsideTests.ps1
```
Expected: lint exits 0; `N test(s) run, 0 failed, 0 crashed` with N = 147 - 1 (RouteTool.DefaultAirframe) + 8 new (SelectTool×2, ScreenPick, InspectFacts, PlanAny×2, DepartAgent×2, Inspector) = 155 or thereabouts. Record N.

- [ ] **Step 2: Log-line count**

Re-run the Task 0 count. Expected: higher than the baseline (Select tool 3, DepartAgent 3, forwarder 1, controller 3, inspector 4, minus 2 from RouteTool). Note exact before/after for the PR.

- [ ] **Step 3: Read the spec once more against the code**

Anything the implementation changed from the spec gets a dated amendment in the spec's own section (§3-§7). Commit as `docs(spec): amendments from execution`.

- [ ] **Step 4: Push and open the PR**

```powershell
git push -u origin feature/entity-inspector
gh pr create --title "feat: entity inspector - select, facts, depart; retires the route tool" --body-file <path to a body written per the PR template: build line, test line, UE_LOG before/after, the two removed log lines named, the AirframeFor spec amendment, and a PIE checklist>
```
The PIE checklist for the user: (1) PIE opens in Select, bar shows Select lit; (2) press 7 to land, click the aircraft on final - panel shows On final, altitude falling; (3) once Parked, Depart lights; click it - log shows `DepartAgent N: Departure: ...` and the aircraft taxis out and goes; (4) click a stand - panel shows code and occupant; (5) press 1, click, right-click twice - back in Select with no selection; (6) C follows the selected aircraft.

- [ ] **Step 5: Update memory**

Update `airportmgr-roadnet-state.md`: route tool retired, entity inspector on `feature/entity-inspector` (PR #), test count, unverified in PIE at PR time, and what PIE verification is pending.
