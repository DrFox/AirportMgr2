# Articulated Rig Step 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import truckCab1 + tankTrailer1 and drive the rig forwards, trailer following, round a pre-laid test course that reports which road tiers it cannot fit.

**Architecture:** ONE trailer stepper in Solve/ (extracted from VehicleSweep::Trace) is called by both route gating and the driving agent. FRoadAgent carries the trailer axle's position; FAgentMotion carries the trailer pose out to ARoadAgentActor, which places a second skeletal mesh. ARigTestCourse lays roads through IRoadEditTarget and loops the rig over them with VehicleFit-gated routes.

**Tech Stack:** UE 5.8 C++, Airside plugin, game module, UE Python (headless import), automation tests.

**Spec:** `docs/superpowers/specs/2026-09-24-articulated-rig-forward-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr-rig`, branch `feature/articulated-rig`. Absolute paths only: the shell cwd resets to `C:\repos\AirportMgr2`, which is the WRONG tree and carries someone's uncommitted fuel-truck script edits. Never touch it.
- Build: `"D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr-rig\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`. Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr-rig\AirportMgr.uproject" -Filter <x>`. Read `N test(s) run, N failed, N crashed`; the baseline is 842/0/0. Check-Architecture must PASS.
- A NEW test .cpp needs TWO builds. The test module is a unity build, so helpers go in a NAMED namespace.
- No Co-Authored-By trailer. Every commit is built first. Comments explain WHY. Tests assert with a named reason.
- Solve/ is CoreMinimal + Solve/ only. Model/ never includes Entities/.
- Content defaults resolve in ONE `UAirsideSettings::Resolve*` function each; no asset path at a second site.
- Rig geometry is `UAirsideSettings::ResolveRigVehicle()` (KingpinX 57.3, KingpinToAxle 1029.5 uu). Never retype it.
- Editor: if an interactive UnrealEditor has `C:\repos\airportmgr-rig\AirportMgr.uproject` open, close it only when its title shows no unsaved `*`; otherwise report BLOCKED. Never touch an editor on `C:\repos\AirportMgr2`.
- Headless Python needs the editor closed. Read the memory notes on import traps: Interchange scale breaks rigs (fix scale in Blender); import_models.py is first-import only; save_asset writes nothing unless forced; the material skeletal-usage flag. Verify the .uasset on disk after any headless write.

## Review Focus

1. The rig spawned mid-bend or not straight: the trailer axle must initialise straight behind the kingpin along the cab heading, never at a stale or zero position.
2. A frame with a large DeltaSeconds: the trailer step must be sub-stepped with the cab (the follower already sub-steps), or the pursuit step overshoots. Pin it with a long-frame test.
3. A rigid vehicle: no trailer component, motion unchanged, and every existing agent test still green.
4. A leg with no fitting route: skipped and logged once per loop, not retried every frame or spammed.
5. PIE duplication: the trailer mesh component on ARoadAgentActor must survive level duplication (memory: transient subobject pointers reset to the CDO on duplication).

---

### Task 1: One trailer stepper

**Files:** `Plugins/Airside/Source/Airside/Public/Solve/VehicleSweep.h`, `Private/Solve/VehicleSweep.cpp` (Trace ~lines 92-125), test `AirsideTests/Private/SweepAgreementTest.cpp` or a new `TrailerStepTest.cpp`.

**Produces:**
```cpp
namespace VehicleSweep
{
	/**
	 * Advance a trailer whose kingpin has moved to Kingpin: the axle is pulled toward it and
	 * kept KingpinToAxle behind (discrete tractrix, the pursuit Trace already used). Returns
	 * false when the trailer has jack-knifed (its heading opposes CabHeading).
	 */
	AIRSIDE_API bool StepTrailer(const FVector2D& Kingpin, const FVector2D& CabHeading,
		double KingpinToAxle, FVector2D& InOutTrailerAxle);

	/** Trailer heading (unit) from axle to kingpin. */
	AIRSIDE_API FVector2D TrailerHeading(const FVector2D& Kingpin, const FVector2D& TrailerAxle);
}
```
Trace's loop (lines ~111-118) calls StepTrailer instead of inlining it; the jack-knife check (`Dot < 0`) moves inside it. The behaviour must be IDENTICAL: every existing VehicleSweep / VehicleFit / road-gating test stays green.

- [ ] Tests, prefix `Airside.Solve.TrailerStep.`:
  - `.StraightStaysStraight`: a kingpin moving along +X in 10 uu steps keeps the axle on the X axis.
  - `.SteadyCircleConverges`: a kingpin on a circle of radius Rk (> KingpinToAxle) settles the axle on the radius `sqrt(Rk² - L²)` within 1 uu. Reason: the analytic steady tractrix.
  - `.JackknifeDetected`: a kingpin reversing straight back returns false.
  - `.TraceUsesTheStepper`: Trace's trailer corners for a 90° path equal a hand loop over StepTrailer (same samples). Reason: one evaluator.
- [ ] Build twice, RED, implement, GREEN; run `-Filter Airside.Solve` and `-Filter Airside.Model` (VehicleFit, gating).
- [ ] Commit `refactor(solve): one trailer stepper, shared by Trace`.

### Task 2: The agent carries the trailer

**Files:** `Public/Model/RoadAgent.h`, `Private/Model/RoadAgent.cpp` (StartDrive ~342, Advance), `Public/Model/AgentMotion.h`, tests in `AirsideTests/Private/SweepAgreementTest.cpp`.

**Produces:**
- On `FRoadAgent`: `UPROPERTY() FVector2D TrailerAxle`, and `bool HasTrailer() const` (Body == Vehicle && Vehicle.HasTrailer()). Initialised in StartDrive straight behind the kingpin along the start heading. The kingpin is the fixed axle + heading * KingpinX. First find which point `FAgentMotion::Position` is (fixed axle or body centre, see DescribeMotion) and derive the kingpin from the right one; state it at the site.
- Stepped in Advance, inside the SAME sub-step loop that moves the cab (never once per frame), through `VehicleSweep::StepTrailer`.
- Jack-knife guard: if StepTrailer returns false, or the hitch angle exceeds `MaxHitchRadians` (a named constant, 90°, with a WHY), the agent stops (reuse the existing stop/refuse path; read how Advance ends a drive) and logs `LogAirside Warning: "Rig %s jack-knifed at (%.0f,%.0f), hitch %.0f deg"`.
- `FAgentMotion` gains `UPROPERTY() bool bHasTrailer = false; UPROPERTY() FVector2D Kingpin; UPROPERTY() double TrailerHeading = 0.0;` (radians, world), filled by DescribeMotion when HasTrailer.
- [ ] Tests, prefix `Airside.Model.Rig.`:
  - `.RigStartsStraight`: StartDrive with ResolveRigVehicle on a straight plan gives hitch 0 and TrailerAxle KingpinToAxle behind the kingpin.
  - `.FollowerMatchesTraceForRig`: extend FollowerMatchesSweep. Drive the rig's agent (with the plan's polyline) through the SweepAgreement 90° turn; the trailer axle samples match VehicleSweep::Trace on the same path within 5 uu. Reason: routing and driving agree.
  - `.LongFrameIsSubStepped`: one Advance of 2 s equals twenty of 0.1 s to within 1 uu (trailer axle). Reason: Review Focus 2.
  - `.RigidHasNoTrailer`: a bowser agent's motion has bHasTrailer false; every existing agent test is unchanged.
  - `.JackknifeStops`: a forced hairpin plan tighter than the lock ends with the agent stopped and the warning logged. Use an unbuffered log spy (memory note).
- [ ] RED, implement, GREEN; run `-Filter Airside.Model` + `-Filter Airside.Present`.
- [ ] Commit `feat(model): the rig's agent carries and steps its trailer`.

### Task 3: Import the rig and resolve its content

**Files:** new `Tools/Python/import_rig.py`, new `Tools/Python/build_rig_anim.py` (mirror `import_fueltruck.py`, `build_fueltruck_anim.py`, `reimport_fueltruck1.py` in the WORKTREE; the main checkout's copies carry uncommitted edits, so read the worktree's), `Public/Content/AirsideSettings.h` + `.cpp`, `Public/Content/AirsideContent.h` (soft pointers for the two meshes and two ABPs), the content asset if one holds them (DA_* content set; find where the fuel truck's mesh/ABP soft pointers live and add beside them), test in `Source/AirportMgr/*ContentTest.cpp` (where PlotKitContentTest lives).

**Produces:** `static FResolvedAgentView UAirsideSettings::ResolveRigView()` or an extension of `ResolveVehicleView(const FVehicle&)` choosing by `HasTrailer()`. Pick the one that keeps ONE function per content default, and justify. It returns the cab mesh + ABP and the trailer mesh + ABP. Assets go under `Content/Vehicles/Rig/` (SK_TruckCab1, SK_TankTrailer1, ABP_TruckCab1, ABP_TankTrailer1), with materials as M_Fleet instances scraped from each glb.

- [ ] Inspect the glbs first (bone names for wheels and steer; the scripts' `wheel_FL_steer` etc.). Check both import at the correct scale: meter-scale glb to cm. The fuel truck's script shows the pipeline's handling. If scale is wrong, fix it in Blender, not import settings; see the memory note.
- [ ] Run headless with the editor closed. Check each .uasset exists on disk afterwards (memory: headless writes that report success).
- [ ] ABPs built by script: the cab gets steer on the front wheels and spin on all wheels; the trailer gets spin on its 4 wheels (read how UAirsideAgentAnim drives the fuel truck's wheels, and reuse its channels).
- [ ] Test `Airside...Content.RigResolves` (game module): both meshes and both ABPs resolve non-null, and the trailer mesh's bounds length is about KingpinToAxle + front + rear, within 10%. Reason: the geometry matches the imported mesh (memory "measured beats typed").
- [ ] Commit the .uassets and scripts: `feat(content): import the articulated rig (truckCab1 + tankTrailer1)`.

### Task 4: The actor shows the trailer

**Files:** `Public/Present/RoadAgentActor.h` + `.cpp`, the presenter that dresses vehicle actors (find the `SetVehicleAirframe` caller), and tests in `AirsideTests/Private/` (actor tests, e.g. beside AgentActor tests).

**Produces:** `void ARoadAgentActor::SetVehicleTrailer(USkeletalMesh* Mesh, UClass* AnimClass);` which creates or updates a `UPROPERTY() TObjectPtr<USkeletalMeshComponent> Trailer` component. It must survive duplication: read the memory note on transient subobject pointers and the existing Airframe component pattern. `SetMotion` places the trailer at `Motion.Kingpin` (world, at SurfaceZ) with yaw `Motion.TrailerHeading`, hidden when `!Motion.bHasTrailer`. The dresser calls SetVehicleTrailer when the resolved view has a trailer.

- [ ] Tests, prefix `Airside.Present.RigActor.`:
  - `.TrailerOnKingpin`: spawn a rig agent actor, tick once; the trailer component's world location XY equals Motion.Kingpin, and its yaw equals TrailerHeading (degrees), within 0.01.
  - `.RigidHasNoTrailerComponent`.
  - `.TrailerSurvivesDuplication`: duplicate the actor (as DuplicatedActorOwnsItsSubobjects does); the copy owns its own trailer component.
- [ ] Commit `feat(present): the rig's trailer drawn at the kingpin`.

### Task 5: The test course and its loop

**Files:** new `Source/AirportMgr/RigTestCourse.h` + `.cpp` (game module: dev tooling, beside BP_RoadBuildGameMode's C++), test `Source/AirportMgr/RigTestCourseTest.cpp`.

**Produces:** `ARigTestCourse : AActor`:
- At BeginPlay it finds the level's ARoadNetworkActor (as ARoadBuildController does) and lays, through IRoadEditTarget, three service-road lanes, one per width tier. Read #274's tier API (`ResolveProfileFor(Kind, WidthIndex)`, the RoadDrawTool width cycling) for how a tier is chosen. Each lane has: a long straight (80 m); left 90 and right 90 corners; a T junction whose stem is driven both ways; a dead end (its U-turn is derived). The lanes are joined into one circuit. Every coordinate is a named constant at the top of the file, with the tier spacing wide enough (60 m) that lanes do not interact.
- `TArray<FRoadNodeId> Waypoints` in visiting order, each with a label ("Standard, right 90").
- Driver: when no rig is out, it plans the leg from Waypoints[i] to Waypoints[i+1] with `FRouteQuery ... .WithVehicle(ResolveRigVehicle())`, dispatches with `UAirsideTraffic`/`GroundTraffic::DispatchAgent(Network, Plan, Rig, ...)` (find the actor-level forwarder), and on arrival advances i. Wraps at the end.
- A refused leg (no Found plan) is skipped, and logged ONCE per loop: `UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) refused: %s"), ...)`, with the refusal reason from the route result and VehicleFit (swept vs tarmac where available). A red DrawDebugString is placed at the leg's start while it stays refused.
- At the loop end: `UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: loop %d - %d/%d legs driven; refused: %s"), ...)`.
- `void BuildCourseForTest(IRoadEditTarget&)` and `int32 LegCountForTest()` for the headless test.

- [ ] Test `AirportMgr.RigCourse.OneLoopHeadless`:
  - Build the course on an FAirsideTestWorld actor.
  - Assert the feature count (3 tiers x 5 features) and that every waypoint node is alive.
  - Drive one full loop at fixed 0.05 s ticks (bounded by a max tick count).
  - Assert every leg is either driven or refused, the refused set equals the legs where planning with WithVehicle(rig) fails, no jack-knife warning fires, and each driven leg ended at its waypoint.
- [ ] Commit `feat(rig): a test course that loops the rig and reports what it cannot fit`.

### Task 6: M_RigTest level, full run, handoff

**Files:** new `Tools/Python/build_rig_test_level.py`, `Content/Maps/M_RigTest.umap`.

- [ ] The script (headless, editor closed) creates M_RigTest with a floor plane, the ARoadNetworkActor, an ARigTestCourse and the world-settings game mode BP_RoadBuildGameMode. Model it on how M_Starter or M_ModelYard was scripted (`build_model_yard.py`). Verify the .umap on disk and that its name table contains RigTestCourse (the memory's grep-the-name-table control).
- [ ] Full suite once: quote the summary line; Check-Architecture PASS.
- [ ] Update the spec with any deviation (REVISED notes). Name the counts of `UE_LOG` and comment lines in VehicleSweep.cpp and RoadAgent.cpp before and after (refactor contract).
- [ ] Commit `feat(rig): M_RigTest level`.
- [ ] PIE handoff, for the user: open M_RigTest, PIE, watch the rig loop, read the `LogRoadBuild: RigCourse: loop` summary.
