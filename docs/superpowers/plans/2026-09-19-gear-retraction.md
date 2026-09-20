# Landing gear retraction and bay doors — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the model a landing-gear cycle with sequenced bay doors, and publish it to Animation Blueprints as two fractions and two ready-to-apply bone angles.

**Architecture:** The model owns the gear and the view copies it, exactly as `FEnginePerformance`/`EngineRPM` already works. A four-state enum plus one elapsed-seconds timer on `FRoadAgent`; both fractions are pure functions of that timer computed by a single evaluator in `Model/`, so the door sequencing falls out of the curve's shape rather than out of extra states. `FAgentMotion` carries the two fractions across to `UAirsideAgentAnim`, which multiplies them by two measured rig angles.

**Tech Stack:** UE 5.8 C++, `Airside` plugin. Automation tests via `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, run headlessly by `Tools/Run-AirsideTests.ps1`. One Python tooling change in `Tools/Python/`.

**Spec:** `docs/superpowers/specs/2026-09-19-gear-retraction-design.md`

## Global Constraints

- **A uu is a centimetre.** Every distance in this plan is uu unless it says otherwise.
- **The editor must be CLOSED for every build.** Live Coding holds the DLLs and the build fails with "Unable to build while Live Coding is active". Every task here adds a `USTRUCT`, `UENUM` or `UPROPERTY`, so **Live Coding covers none of it** — `Ctrl+Alt+F11` is not an option at any point in this plan.
- **The build line** (from CLAUDE.md), run from `C:\repos\AirportMgr2`:
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- **A NEW TEST `.cpp` NEEDS TWO BUILDS.** The first reports `Result: Succeeded` without compiling it, and the test then does not exist to run. Task 1 creates the only new test file in this plan and its steps say so explicitly.
- **Never trust the automation runner's exit code.** Read its `N test(s) run, N failed, N crashed` line; a crashing test used to vanish and report green.
- **Test names must be distinct leaves.** UE's automation tree drops a bare-named parent once a dotted child exists — `Airside.Model.LandingRun` and `Airside.Model.LandingRun.Something` already collide this way in the suite. Every name in this plan is a distinct leaf with no children.
- **`Tools/Check-Architecture.ps1` is the pre-commit lint** and runs first inside the test script.
- **Comments explain WHY**, and especially why an obvious alternative was rejected. Match the surrounding density; the comment blocks in this plan's code are part of the deliverable, not decoration.
- **Do not add a `Co-Authored-By` trailer** beyond the one the harness specifies, and branch names are `feature/*`.

**Values fixed by the spec, copied verbatim:**

| Constant | Value |
|---|---|
| `FGearPerformance::TravelSeconds` (737) | `7.0` |
| `FGearPerformance::DoorSeconds` (737) | `1.0` |
| `FGearPerformance::RetractAboveHeight` (737) | `9000.0` |
| `FGearPerformance::ExtendBelowHeight` (737) | `15000.0` |
| `UAirsideAgentAnim::GearRetractedAngleDegrees` | `90.0f` |
| `UAirsideAgentAnim::BayDoorOpenAngleDegrees` | `81.0f` |

---

### Task 1: `FGearPerformance`, `EGearPhase`, and the single evaluator

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` — insert after `FEnginePerformance` (which closes at the line `};` following its `IsSet()`, immediately before the `/** How an airframe MOVES on the ground.` doc block)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadEntity.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp` (create)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class EGearPhase : uint8 { Down, Raising, Up, Lowering }`
  - `struct FGearPerformance` with `double TravelSeconds`, `double DoorSeconds`, `double RetractAboveHeight`, `double ExtendBelowHeight`
  - `bool FGearPerformance::IsSet() const`
  - `double FGearPerformance::CycleSeconds() const`
  - `void FGearPerformance::FractionsAt(double Elapsed, bool bRaising, double& OutGearDown, double& OutDoorOpen) const`
  - `FAirframe::Gear` (`FGearPerformance`) — Task 2 reads it, so it is added here

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearCycleSequencesDoorsTest,
	"Airside.Model.GearCycleSequencesDoors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearCycleSequencesDoorsTest::RunTest(const FString& Parameters)
{
	// 1 s of door travel either side of 7 s of gear travel - the 737 figures, so the numbers
	// below are the ones that will actually be flown rather than a convenient round set.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	TestEqual(TEXT("the cycle is the gear travel bracketed by a door movement each side"),
		Gear.CycleSeconds(), 9.0);

	double GearDown = -1.0;
	double DoorOpen = -1.0;

	// THE GEAR DOES NOT MOVE UNTIL THE DOORS ARE OPEN. This is the whole sequencing decision
	// and it is asserted at the last instant before the gear is allowed to travel, not in the
	// middle of the door stage where a partly-working implementation would also pass.
	Gear.FractionsAt(0.99, /*bRaising*/ true, GearDown, DoorOpen);
	TestEqual(TEXT("at 0.99 s the gear is still fully down and locked"), GearDown, 1.0);
	TestTrue(TEXT("while the doors are nearly but not quite open"),
		DoorOpen > 0.9 && DoorOpen < 1.0);

	// AND IT HAS MOVED BY THE MIDDLE. Without this the assertion above passes perfectly on a
	// gear that never moves at all - which is the failure a-green-test-may-measure-nothing
	// records, and the reason every case in this file pins a value that must CHANGE.
	Gear.FractionsAt(4.5, true, GearDown, DoorOpen);
	TestTrue(TEXT("half way through the travel the gear is part way up"),
		GearDown > 0.4 && GearDown < 0.6);
	TestEqual(TEXT("and the doors are held fully open across the whole travel"), DoorOpen, 1.0);

	// THE DOORS DO NOT CLOSE UNTIL THE GEAR IS STOWED. The other half of the sequencing, at
	// the instant the gear arrives.
	Gear.FractionsAt(8.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("at 8 s the gear is fully up"), GearDown, 0.0);
	TestEqual(TEXT("and only now may the doors begin to close"), DoorOpen, 1.0);

	// AT REST. Doors shut over a stowed wheel, which is what a 737 does and why plane4 has
	// door_nose_L/_R at all.
	Gear.FractionsAt(9.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("the cycle ends with the gear up"), GearDown, 0.0);
	TestEqual(TEXT("and the bay shut over it"), DoorOpen, 0.0);

	// PAST THE END IS STILL THE END. A timer that overran used to be a source of flicker in
	// this project's other integrators; clamped rather than wrapped.
	Gear.FractionsAt(100.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("an overrun timer holds the resting pose"), GearDown, 0.0);
	TestEqual(TEXT("and does not reopen the doors"), DoorOpen, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearExtendMirrorsRetractTest,
	"Airside.Model.GearExtendMirrorsRetract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearExtendMirrorsRetractTest::RunTest(const FString& Parameters)
{
	// THE SYMMETRY IS THE ARGUMENT FOR BUILDING THE EXTEND HALF AT ALL. Nothing can currently
	// see it - an arrival joins final at FApproachPerformance::FinalAltitude, 2000 uu, about
	// 66 ft, already configured - so this test is what stops it rotting unnoticed.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	double UpGearDown = -1.0;
	double UpDoorOpen = -1.0;
	double DownGearDown = -1.0;
	double DownDoorOpen = -1.0;

	for (const double At : {0.0, 0.5, 1.0, 4.5, 8.0, 8.5, 9.0})
	{
		Gear.FractionsAt(At, /*bRaising*/ true, UpGearDown, UpDoorOpen);
		Gear.FractionsAt(At, /*bRaising*/ false, DownGearDown, DownDoorOpen);

		TestEqual(FString::Printf(TEXT("the doors do the same thing either way at %.1f s"), At),
			DownDoorOpen, UpDoorOpen);
		TestEqual(FString::Printf(TEXT("and the gear is the exact complement at %.1f s"), At),
			DownGearDown, 1.0 - UpGearDown);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearWithoutDoorsStillTravelsTest,
	"Airside.Model.GearWithoutDoorsStillTravels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearWithoutDoorsStillTravelsTest::RunTest(const FString& Parameters)
{
	// DoorSeconds = 0 means "no bay doors", not "instant doors". An airframe whose mains sit
	// behind a fixed fairing - which is every main gear in this fleet - still retracts.
	FGearPerformance Gear;
	Gear.TravelSeconds = 4.0;
	Gear.DoorSeconds = 0.0;

	TestEqual(TEXT("with no doors the cycle is the travel alone"), Gear.CycleSeconds(), 4.0);

	double GearDown = -1.0;
	double DoorOpen = -1.0;

	Gear.FractionsAt(2.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("the gear is half way up at the half way point"), GearDown, 0.5);
	TestEqual(TEXT("and no door ever opens, rather than one snapping open"), DoorOpen, 0.0);

	// DIVISION BY DoorSeconds IS THE HAZARD HERE and this is what proves it is guarded: a
	// NaN fraction does not show up as a stuck door, it shows up as a bone transform that
	// makes the whole aeroplane vanish.
	Gear.FractionsAt(0.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("and the first frame is a number, not a NaN"), GearDown, 1.0);

	return true;
}

#endif
```

- [ ] **Step 2: Build once so the new file is discovered, then run to verify the test fails**

A new `.cpp` in this module needs **two** builds — the first reports `Result: Succeeded` without compiling it.

Run, twice:
```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```
Expected: the **second** build FAILS to compile with `'FGearPerformance': undeclared identifier` in `GearCycleTest.cpp`. That failure is the red state — the test cannot run until Step 3 exists.

- [ ] **Step 3: Add the enum and the struct**

In `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h`, immediately after `FEnginePerformance`'s closing `};` and before the `/** How an airframe MOVES on the ground.` block:

```cpp
/**
 * Where the landing gear has got to.
 *
 * A PHASE IS AN ENUM, NEVER A SET OF BOOLS - CLAUDE.md's rule, and it bites hard here.
 * bGearUp plus bDoorsOpen plus bInTransit can express "up, in transit, doors shut", which is
 * the state where an aeroplane's wheels are passing through its own bay doors.
 *
 * FOUR STATES AND NOT EIGHT. A sequenced cycle is doors-open, gear-travel, doors-close, and
 * the obvious encoding gives each stage its own phase in each direction. The timer already
 * says which stage it is in - see FGearPerformance::FractionsAt - so the extra four would be
 * a second answer to a question one double already answers.
 */
UENUM()
enum class EGearPhase : uint8
{
	/** Down and locked. Every phase on the ground, and every airframe with fixed gear. */
	Down,

	/** In transit upward: doors opening, gear travelling, doors closing. */
	Raising,

	/** Up and stowed, bay doors shut over it. */
	Up,

	/** In transit downward. The same timeline as Raising, read the other way. */
	Lowering
};

/**
 * How an airframe's landing gear retracts, and how long its bay doors take.
 *
 * Shaped like FEnginePerformance beside it, including the IsSet() idiom, and for the same
 * reason: this is authored per type and read by the model, and an airframe that has not
 * declared it must behave rather than crash.
 *
 * ZERO TravelSeconds MEANS FIXED GEAR - a fact about the aeroplane - and NOT "unmeasured",
 * which is what zero means on FAirframe::MainGearTrack. The difference is that there is no
 * third possibility to confuse it with: an aeroplane either retracts its gear or it does
 * not, and a DHC-6 Twin Otter does not. plane2 is correct by construction and permanently.
 *
 * RETRACTION IS A PILOT COMMAND, NOT A CONSEQUENCE OF LIFT-OFF. It is called for a few
 * hundred feet above the ground, which is why RetractAboveHeight exists and why
 * FAgentMotion::bAirborne is the PRECONDITION rather than the cue - contradicting that
 * flag's own comment, which predicted the cycle would simply hang off it.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FGearPerformance
{
	GENERATED_BODY()

	/** Gear travel, seconds. ZERO MEANS FIXED GEAR - see the struct comment. */
	UPROPERTY(EditAnywhere) double TravelSeconds = 0.0;

	/**
	 * One bay door movement, seconds. Zero means the airframe has no doors to move.
	 *
	 * COUNTED ONCE BUT SPENT TWICE - the doors open before the gear travels and close after
	 * it, so CycleSeconds() is this plus the travel plus this again. Authored as one figure
	 * because a door takes the same time to open as to shut.
	 */
	UPROPERTY(EditAnywhere) double DoorSeconds = 0.0;

	/**
	 * Height above the surface at which a departure raises its gear, uu.
	 *
	 * READ ONLY WHILE DEPARTING - see FRoadAgent::AdvanceGear. ExtendBelowHeight sits ABOVE
	 * this figure, so an arrival descending through it is "airborne and above the retract
	 * height" and would raise its gear on short final if altitude alone decided.
	 */
	UPROPERTY(EditAnywhere) double RetractAboveHeight = 9000.0;

	/**
	 * Height below which an arrival lowers its gear, uu.
	 *
	 * LATENT AT TODAY'S FIGURES. An arrival joins final at FApproachPerformance::
	 * FinalAltitude, 2000 uu, which is below this - so every arrival is born down and locked
	 * and the extension is never on screen. It becomes visible the day the approach is joined
	 * higher, which is the whole argument for building the half nobody can see.
	 */
	UPROPERTY(EditAnywhere) double ExtendBelowHeight = 15000.0;

	/** Has anyone declared retractable gear for this airframe? */
	bool IsSet() const { return TravelSeconds > 0.0; }

	/** Doors out, gear across, doors back. Seconds. */
	double CycleSeconds() const
	{
		return FMath::Max(DoorSeconds, 0.0) * 2.0 + FMath::Max(TravelSeconds, 0.0);
	}

	/**
	 * Both fractions at a point in a cycle. OutGearDown is 1 down-and-locked, 0 stowed;
	 * OutDoorOpen is 0 shut, 1 fully open.
	 *
	 * THE ONE EVALUATOR. The model stores what this returns and the view draws it; nothing
	 * re-derives either number. That is the guideline graph's invariant applied to a second
	 * place - a second evaluator lets the doors the player sees disagree with the doors the
	 * model thinks it opened, visibly and only mid-cycle.
	 *
	 * Const and free of any agent, so a whole cycle can be sampled in a loop with no world,
	 * no actor and no skeleton.
	 */
	void FractionsAt(double Elapsed, bool bRaising, double& OutGearDown, double& OutDoorOpen) const;
};
```

Then add the member to the `FAirframe` bundle (same header, further down), after
`UPROPERTY(EditAnywhere) FEnginePerformance Engine;`:

```cpp
	/** How this airframe's gear retracts, and whether it does at all. See FGearPerformance. */
	UPROPERTY(EditAnywhere) FGearPerformance Gear;
```

**This member belongs to Task 1 even though nothing reads it until Task 2**, because
`FGearPerformance` and the bundle that carries it are one declaration in one header and
splitting them across two commits leaves the tree with a struct nothing can reach.

- [ ] **Step 4: Implement the evaluator**

Append to `Plugins/Airside/Source/Airside/Private/Model/RoadEntity.cpp`:

```cpp
void FGearPerformance::FractionsAt(double Elapsed, bool bRaising,
	double& OutGearDown, double& OutDoorOpen) const
{
	if (!IsSet())
	{
		// Nothing authored, so there is no shape to sample. AdvanceGear checks IsSet() before
		// it ever gets here, but the resting pose is the right answer anyway: a gear stuck
		// half-retracted is a worse failure than one that never moves.
		OutGearDown = bRaising ? 0.0 : 1.0;
		OutDoorOpen = 0.0;
		return;
	}

	const double Doors = FMath::Max(DoorSeconds, 0.0);
	const double Travel = FMath::Max(TravelSeconds, 0.0);

	// CLAMPED, NOT WRAPPED. A cycle that has run past its end holds the pose it arrived in;
	// wrapping would send the gear back down the moment it finished coming up.
	const double At = FMath::Clamp(Elapsed, 0.0, Doors + Travel + Doors);

	// STAGE 2 IS THE ONE THAT MOVES THE GEAR, bracketed by the two door stages. THAT interval
	// is the whole sequencing decision - expressed as a pair of bounds rather than as four
	// extra enum states. Doors = 0 collapses both brackets to nothing and the gear simply
	// travels, which is what an airframe with no bay doors wants.
	const double GearStart = Doors;
	const double GearEnd = Doors + Travel;

	const double Progress = Travel > 0.0
		? FMath::Clamp((At - GearStart) / Travel, 0.0, 1.0)
		: (At >= GearStart ? 1.0 : 0.0);

	// RAISING COUNTS DOWN FROM 1, LOWERING COUNTS UP TO IT. One timeline and one direction
	// flag, which is what makes the extend half nearly free - and free is the argument that
	// justified building a half nothing can currently see.
	OutGearDown = bRaising ? 1.0 - Progress : Progress;

	if (Doors <= 0.0)
	{
		// NO DOORS, and this branch is also what keeps the divisions below away from zero.
		OutDoorOpen = 0.0;
		return;
	}

	// THE TRAPEZOID: open across the first Doors seconds, HELD open for the whole of the
	// gear's travel, shut across the last Doors seconds. Holding it open for the travel is
	// what stops the wheel passing through a door that seals to 0.0 mm on plane4.
	if (At < GearStart)
	{
		OutDoorOpen = FMath::Clamp(At / Doors, 0.0, 1.0);
	}
	else if (At < GearEnd)
	{
		OutDoorOpen = 1.0;
	}
	else
	{
		OutDoorOpen = FMath::Clamp(1.0 - (At - GearEnd) / Doors, 0.0, 1.0);
	}
}
```

- [ ] **Step 5: Build and run the tests**

Run the build line, then:
```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Gear
```
Expected: `3 test(s) run, 0 failed, 0 crashed`. **Read that line** — do not read the exit code.

- [ ] **Step 6: Prove the tests measure something**

Temporarily change `FractionsAt`'s door branch so the doors are held open for the whole cycle (replace the final `else` body with `OutDoorOpen = 1.0;`). Rebuild and re-run.

Expected: `Airside.Model.GearCycleSequencesDoors` FAILS on "and the bay shut over it". Revert the change and rebuild before committing. A test whose rule you have not deleted and watched go red is a test that may be measuring nothing.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h Plugins/Airside/Source/Airside/Private/Model/RoadEntity.cpp Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp
git commit -m "feat(model): a gear cycle is one timer, four phases and one evaluator"
```

---

### Task 2: The agent's gear state and the height cue

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h` — add two `UPROPERTY`s beside `EngineRPM` (line 320), and declare `AdvanceGear`/`GearFractions` beside `AdvanceEngine` (line 528)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp` — implement both; call `AdvanceGear` in `Advance` immediately after the existing `AdvanceEngine(DeltaSeconds);` (line 255)
- Test: `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp` (extend — no new file, so one build)

**Interfaces:**
- Consumes: `EGearPhase`, `FGearPerformance::IsSet/CycleSeconds/FractionsAt` from Task 1.
- Produces:
  - `FRoadAgent::GearPhase` (`EGearPhase`), `FRoadAgent::GearCycleSeconds` (`double`)
  - `void FRoadAgent::AdvanceGear(double DeltaSeconds)`
  - `void FRoadAgent::GearFractions(double& OutGearDown, double& OutDoorOpen) const`

- [ ] **Step 1: Write the failing tests**

Append to `GearCycleTest.cpp`, inside the `#if WITH_DEV_AUTOMATION_TESTS` block (before the closing `#endif`), and add `#include "Model/RoadAgent.h"` to the top of the file beside the existing includes:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearRetractsAtHeightNotLiftOffTest,
	"Airside.Model.GearRetractsAtHeightNotLiftOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearRetractsAtHeightNotLiftOffTest::RunTest(const FString& Parameters)
{
	// THE CORRECTION THIS WHOLE FEATURE TURNS ON. Retraction is a pilot command given a few
	// hundred feet up, not something that happens when the wheels leave the tarmac -
	// FAgentMotion::bAirborne's own comment ("Stage 2's gear retraction hangs on this")
	// predicted otherwise and is what this test exists to contradict.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;
	Agent.Airframe.Gear.RetractAboveHeight = 9000.0;
	Agent.Phase = EAgentPhase::Departing;

	// AIRBORNE BUT LOW. The rotation is already guarded by Airside.Present.AgentMotion case
	// 4, which asserts a rotating aircraft is not airborne at all; this is the next question
	// along - airborne, climbing, and still below the height the gear comes up at.
	Agent.LastMotion.bAirborne = true;
	Agent.LastMotion.Altitude = 3000.0;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("airborne at 30 m the gear has not started up"),
		Agent.GearPhase, EGearPhase::Down);

	// PAST THE CUE. Nothing about the aircraft has changed except its height.
	Agent.LastMotion.Altitude = 9500.0;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("past the retract height the cycle begins"),
		Agent.GearPhase, EGearPhase::Raising);

	// AND IT ARRIVES. Flown out over the whole 9 s cycle in 0.5 s steps, with one spare.
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Agent.AdvanceGear(0.5);
	}
	TestEqual(TEXT("and the gear ends up stowed"), Agent.GearPhase, EGearPhase::Up);

	double GearDown = -1.0;
	double DoorOpen = -1.0;
	Agent.GearFractions(GearDown, DoorOpen);
	TestEqual(TEXT("reporting nothing left down"), GearDown, 0.0);
	TestEqual(TEXT("behind a shut bay"), DoorOpen, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearDescendingArrivalDoesNotRetractTest,
	"Airside.Model.GearDescendingArrivalDoesNotRetract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearDescendingArrivalDoesNotRetractTest::RunTest(const FString& Parameters)
{
	// THE DEFECT THE TWO HEIGHTS INVITE. ExtendBelowHeight (15000) sits ABOVE
	// RetractAboveHeight (9000), so an arrival descending through 9000 uu satisfies
	// "airborne and above the retract height" word for word. If altitude alone decided, a
	// landing aeroplane would raise its gear on short final.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;
	Agent.Airframe.Gear.RetractAboveHeight = 9000.0;
	Agent.Airframe.Gear.ExtendBelowHeight = 15000.0;

	Agent.Phase = EAgentPhase::Arriving;
	Agent.LastMotion.bAirborne = true;
	Agent.LastMotion.Altitude = 9500.0;

	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("an arrival descending through the retract height keeps its gear down"),
		Agent.GearPhase, EGearPhase::Down);

	// AND THE SAME HEIGHT ON A DEPARTURE DOES RETRACT, which is what proves the phase is
	// what discriminates rather than something incidental about the arrival.
	Agent.Phase = EAgentPhase::Departing;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("while a departure at that exact height raises it"),
		Agent.GearPhase, EGearPhase::Raising);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearFixedWhenUnauthoredTest,
	"Airside.Model.GearFixedWhenUnauthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearFixedWhenUnauthoredTest::RunTest(const FString& Parameters)
{
	// plane2's Twin Otter, and every ground vehicle. An unauthored FGearPerformance is a
	// statement that the gear is FIXED, not that nobody has measured it - so it must stay
	// down through a whole climb rather than snapping up at the cue height.
	FRoadAgent Agent;
	Agent.Phase = EAgentPhase::Departing;
	Agent.LastMotion.bAirborne = true;

	for (int32 Step = 0; Step < 60; ++Step)
	{
		// Climbing steadily past every height in the spec, including ClearAltitude.
		Agent.LastMotion.Altitude = Step * 500.0;
		Agent.AdvanceGear(0.5);
	}

	TestEqual(TEXT("fixed gear never leaves the down phase"), Agent.GearPhase, EGearPhase::Down);

	double GearDown = -1.0;
	double DoorOpen = -1.0;
	Agent.GearFractions(GearDown, DoorOpen);
	TestEqual(TEXT("and reports itself fully down for the whole flight"), GearDown, 1.0);
	TestEqual(TEXT("with no door it does not have"), DoorOpen, 0.0);

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run the build line.
Expected: FAILS to compile — `'GearPhase': is not a member of 'FRoadAgent'`.

- [ ] **Step 3: Add the state and declare the methods**

In `Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h`, immediately after the `UPROPERTY() double EngineRPM = 0.0;` member:

```cpp
	/**
	 * Where the gear has got to. Advanced by AdvanceGear every frame, whichever phase is
	 * driving - the same arrangement as EngineRPM above and for the same reason.
	 */
	UPROPERTY() EGearPhase GearPhase = EGearPhase::Down;

	/**
	 * How far into a raise or a lower, seconds. Meaningless in Down and Up, and reset to
	 * zero on arrival at either so it cannot be read as a stale position.
	 */
	UPROPERTY() double GearCycleSeconds = 0.0;
```

And immediately after the `void AdvanceEngine(double DeltaSeconds);` declaration:

```cpp
	/**
	 * Moves the gear one frame, and starts or finishes a cycle when the aircraft passes a
	 * cue height.
	 *
	 * Beside AdvanceEngine and called next to it for the same reason: it happens in ALL
	 * phases. A cycle that only advanced inside the Departing branch would freeze the doors
	 * half open the moment a departure handed over.
	 *
	 * READS LastMotion.Altitude, which is LAST frame's height. A frame of lag on a cue that
	 * is crossed once per flight is not worth restructuring Advance for - the alternative is
	 * moving this call below a switch that returns early in four of its branches.
	 */
	void AdvanceGear(double DeltaSeconds);

	/** Both gear fractions - see FGearPerformance::FractionsAt, which produces them. */
	void GearFractions(double& OutGearDown, double& OutDoorOpen) const;
```

- [ ] **Step 4: Implement both, and call `AdvanceGear`**

In `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp`, after `AdvanceEngine`:

```cpp
void FRoadAgent::AdvanceGear(double DeltaSeconds)
{
	if (!Airframe.Gear.IsSet())
	{
		// FIXED GEAR, permanently - see FGearPerformance, where zero travel means a fact
		// about the aeroplane rather than a missing measurement. Asserted rather than left
		// alone because an FRoadAgent is reused across dispatches and could otherwise carry
		// a retracted phase into an airframe that has no way of lowering it again.
		GearPhase = EGearPhase::Down;
		GearCycleSeconds = 0.0;
		return;
	}

	// THE COMMAND - what the pilot has called for, not where the gear has got to. The two are
	// separate for the same reason FAgentMotion::bEngineRunning and EngineRPM are: between a
	// command and its completion there are nine seconds in which they disagree.
	//
	// READ BY PHASE, NOT BY ALTITUDE ALONE. ExtendBelowHeight sits ABOVE RetractAboveHeight,
	// so an arrival descending through the retract height satisfies "airborne and above it"
	// word for word and would raise its gear on short final. The model already knows which
	// way the aeroplane is going; asking the altitude to tell us would re-derive it.
	bool bWantUp = false;
	if (Phase == EAgentPhase::Departing)
	{
		// bAirborne IS THE PRECONDITION AND HEIGHT IS THE CUE. Airside.Present.AgentMotion
		// case 4 guards the half that matters most - a rotation is not airborne - so nothing
		// here can start moving while the mains are still carrying the aeroplane.
		bWantUp = LastMotion.bAirborne
			&& LastMotion.Altitude >= Airframe.Gear.RetractAboveHeight;
	}
	else if (Phase == EAgentPhase::Arriving)
	{
		// LATENT AT TODAY'S FIGURES: an arrival joins final at FinalAltitude, 2000 uu, well
		// below ExtendBelowHeight, so this is false from the first frame and every arrival is
		// born down and locked. It starts mattering the day the approach is joined higher.
		bWantUp = LastMotion.Altitude > Airframe.Gear.ExtendBelowHeight;
	}
	// Every other phase is on the ground, where the gear is down by definition.

	const bool bMoving = GearPhase == EGearPhase::Raising || GearPhase == EGearPhase::Lowering;

	// A CYCLE IN PROGRESS IS NOT INTERRUPTED. Real gear can be reversed mid-travel; nothing
	// in this game commands that, and honouring it would mean carrying the position across a
	// direction change rather than restarting a timer at zero. Left out deliberately.
	if (!bMoving && bWantUp != (GearPhase == EGearPhase::Up))
	{
		GearPhase = bWantUp ? EGearPhase::Raising : EGearPhase::Lowering;
		GearCycleSeconds = 0.0;
		return;
	}

	if (!bMoving)
	{
		return;
	}

	GearCycleSeconds += DeltaSeconds;
	if (GearCycleSeconds >= Airframe.Gear.CycleSeconds())
	{
		// THE PHASE SAYS WHERE IT IS AT REST, not the timer. A timer left running past the
		// end would answer correctly right up until anything reset it.
		GearPhase = GearPhase == EGearPhase::Raising ? EGearPhase::Up : EGearPhase::Down;
		GearCycleSeconds = 0.0;
	}
}

void FRoadAgent::GearFractions(double& OutGearDown, double& OutDoorOpen) const
{
	// THE RESTING POSES ARE ANSWERED HERE AND NOT BY THE EVALUATOR, because a resting pose is
	// not a point in a cycle - a fixed-gear airframe has no cycle to sample at all.
	switch (GearPhase)
	{
	case EGearPhase::Down: OutGearDown = 1.0; OutDoorOpen = 0.0; return;
	case EGearPhase::Up:   OutGearDown = 0.0; OutDoorOpen = 0.0; return;
	default: break;
	}

	Airframe.Gear.FractionsAt(GearCycleSeconds, GearPhase == EGearPhase::Raising,
		OutGearDown, OutDoorOpen);
}
```

And in `FRoadAgent::Advance`, immediately after the existing `AdvanceEngine(DeltaSeconds);`:

```cpp
	// AND THE GEAR, for the same reason and on the same terms - see AdvanceGear. A cycle that
	// only ran inside one phase's branch would freeze the doors half open at a handover.
	AdvanceGear(DeltaSeconds);
```

- [ ] **Step 5: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Gear
```
Expected: `6 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp
git commit -m "feat(model): the gear comes up at a height, not at lift-off"
```

---

### Task 3: Carry the fractions across to the view

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` — add two members to `FAgentMotion`, after `SteerAngleDegrees` (line 257) and before the struct's closing `};`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp` — `DescribeMotion`, after the `Motion.bAirborne = ...` assignment
- Test: `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadAgent::GearFractions` from Task 2.
- Produces: `FAgentMotion::GearDownFraction` (`double`, 1.0 = down and locked), `FAgentMotion::BayDoorOpenFraction` (`double`, 0.0 = shut).

- [ ] **Step 1: Write the failing test**

Append to `GearCycleTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearReachesTheMotionDescriptionTest,
	"Airside.Model.GearReachesTheMotionDescription",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearReachesTheMotionDescriptionTest::RunTest(const FString& Parameters)
{
	// THE SEAM. FAgentMotion is everything the view is told, so a fraction the model computes
	// and does not publish here is a fraction no Animation Blueprint can ever read.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;

	// AT REST FIRST, because "down and locked" is what every taxiing aeroplane reports and it
	// is the value a broken default would most plausibly be mistaken for.
	const FAgentMotion Parked = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("a parked aircraft reports its gear down"), Parked.GearDownFraction, 1.0);
	TestEqual(TEXT("and its bay shut"), Parked.BayDoorOpenFraction, 0.0);

	// MID-CYCLE, where the two disagree with both resting poses - which is the only place a
	// forwarder that returned a constant would be caught.
	Agent.GearPhase = EGearPhase::Raising;
	Agent.GearCycleSeconds = 4.5;

	const FAgentMotion Climbing = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestTrue(TEXT("half way up the gear is part way retracted"),
		Climbing.GearDownFraction > 0.4 && Climbing.GearDownFraction < 0.6);
	TestEqual(TEXT("with the bay held fully open around it"),
		Climbing.BayDoorOpenFraction, 1.0);

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run the build line.
Expected: FAILS to compile — `'GearDownFraction': is not a member of 'FAgentMotion'`.

- [ ] **Step 3: Add the fields**

In `FAgentMotion`, after `UPROPERTY() double SteerAngleDegrees = 0.0;`:

```cpp
	/**
	 * Where the landing gear is: 1 down and locked, 0 stowed. See FGearPerformance.
	 *
	 * A FRACTION AND NOT AN ANGLE, because the travel angle is a fact about one RIG - 90
	 * degrees on plane4, measured in its build_export.py - and the model has no business
	 * knowing it. UAirsideAgentAnim multiplies by its own measured figure, the same split
	 * MainWheelRadius already makes.
	 *
	 * ONE DEFAULTS TO DOWN, deliberately: an airframe with no gear data, a vehicle, and every
	 * aircraft on the ground all want the same answer, and it is this one.
	 */
	UPROPERTY() double GearDownFraction = 1.0;

	/** The gear bay doors: 0 shut, 1 fully open. Zero for an airframe with no doors. */
	UPROPERTY() double BayDoorOpenFraction = 0.0;
```

- [ ] **Step 4: Publish them**

In `DescribeMotion`, after the `Motion.bAirborne = ...` assignment:

```cpp
	// THE GEAR, BOTH NUMBERS FROM THE ONE EVALUATOR - see FGearPerformance::FractionsAt. The
	// view applies these to bones and derives neither of them; a second evaluator would let
	// the doors the player sees disagree with the doors the model thinks it opened.
	GearFractions(Motion.GearDownFraction, Motion.BayDoorOpenFraction);

- [ ] **Step 5: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Gear
```
Expected: `7 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp
git commit -m "feat(model): FAgentMotion carries the gear and the bay doors"
```

---

### Task 4: Author the 737's gear

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h` — a member after `FEnginePerformance Engine;` (line 146), and one line in `Airframe()` (after `Out.Engine = Engine;`)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp` — `Build737`, after its `Ground` figures (line 170)
- Test: `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp` (extend; add `#include "Entities/AircraftType.h"`)

**Interfaces:**
- Consumes: `FGearPerformance` from Task 1, and `FAirframe::Gear`, which Task 1 Step 3 added.
- Produces: `UAircraftType::Gear` (`FGearPerformance`), carried into `FAirframe::Gear` by `UAircraftType::Airframe()`.

- [ ] **Step 1: Write the failing test**

Append to `GearCycleTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGear737IsAuthoredAndTravelsTest,
	"Airside.Model.Gear737IsAuthoredAndTravels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGear737IsAuthoredAndTravelsTest::RunTest(const FString& Parameters)
{
	// THE ONLY AIRFRAME AUTHORED. plane4's rig is the only one in the fleet with gear_* and
	// door_nose_* bones - plane2's and plane3's are root, nosewheel_steer, nosewheel, prop_L,
	// prop_R, wheel_L, wheel_R and nothing else - so data on any other type would be data
	// nothing can consume, which reads as working.
	UAircraftType* Type = NewObject<UAircraftType>();
	UAircraftType::Build737(Type);

	const FAirframe Frame = Type->Airframe();

	TestTrue(TEXT("the 737 has retractable gear"), Frame.Gear.IsSet());
	TestEqual(TEXT("with the real transit time"), Frame.Gear.TravelSeconds, 7.0);
	TestEqual(TEXT("and a second of nose bay door each side"), Frame.Gear.DoorSeconds, 1.0);
	TestEqual(TEXT("making a nine second cycle"), Frame.Gear.CycleSeconds(), 9.0);

	// THE CUE HEIGHTS TRAVEL WITH IT, and their ORDER is the thing worth pinning: the extend
	// height sits ABOVE the retract height, which is exactly what makes altitude alone an
	// unsafe discriminator. See Airside.Model.GearDescendingArrivalDoesNotRetract.
	TestEqual(TEXT("gear up passing 9000 uu"), Frame.Gear.RetractAboveHeight, 9000.0);
	TestEqual(TEXT("gear down below 15000 uu"), Frame.Gear.ExtendBelowHeight, 15000.0);
	TestTrue(TEXT("and the extend height is the higher of the two"),
		Frame.Gear.ExtendBelowHeight > Frame.Gear.RetractAboveHeight);

	// A PIPER IS NOT AUTHORED, which is the other half of the claim. Its rig cannot show a
	// retraction, so it declares none - and this is what catches a later edit that copies
	// gear figures onto every type "for completeness".
	UAircraftType* Piper = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Piper);
	TestFalse(TEXT("the Meridian declares no gear cycle, because its rig cannot show one"),
		Piper->Airframe().Gear.IsSet());

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run the build line.
Expected: FAILS to compile — `'Gear': is not a member of 'UAircraftType'`.

- [ ] **Step 3: Add the type member and carry it into the bundle**

In `AircraftType.h`, after `UPROPERTY(EditAnywhere) FEnginePerformance Engine;`:

```cpp
	/**
	 * How this type's landing gear retracts, and whether it does at all.
	 *
	 * UNSET ON EVERY TYPE BUT THE 737, and that is a statement about the RIGS rather than
	 * about the aeroplanes. A Dash 8 and a Meridian both retract in reality; neither mesh has
	 * a bone that could show it, and authored data nothing can consume reads as working. Each
	 * gets figures the same day its rig gets gear_* bones, and not before.
	 */
	UPROPERTY(EditAnywhere) FGearPerformance Gear;
```

In `Airframe()`, after `Out.Engine = Engine;`:

```cpp
		Out.Gear = Gear;
```

- [ ] **Step 4: Author the figures**

In `AircraftType.cpp`, at the end of `Build737` after the `Ground` block:

```cpp
	// THE ONLY TYPE IN THE FLEET WITH GEAR FIGURES, because plane4 is the only mesh with the
	// bones to show them - gear_nose, gear_L, gear_R retract and door_nose_L/_R hinge. The
	// mains have no doors and want none: a 737's main wheels sit in a well behind a fixed
	// fairing, which is why the rig has no door_main_* pair to drive.
	//
	// 7 seconds is the real 737-800 transit. 1 second of door each side of it, so a full
	// cycle is 9 - long enough to be watched, and comfortably inside a climb that does not
	// reach ClearAltitude for the better part of a minute.
	Type->Gear.TravelSeconds = 7.0;
	Type->Gear.DoorSeconds = 1.0;

	// A FEW HUNDRED FEET, which is when the command is actually given - 9000 uu is about 295
	// ft. NOT lift-off: FAgentMotion::bAirborne is the precondition and this is the cue, and
	// the two were conflated in bAirborne's own comment until this landed.
	Type->Gear.RetractAboveHeight = 9000.0;

	// ABOVE FApproachPerformance::FinalAltitude (2000 uu), so an arrival is born down and
	// locked and this never fires at today's figures. Authored at an honest ~500 ft anyway,
	// so the extension is correct the day the approach is joined higher rather than being
	// discovered missing.
	Type->Gear.ExtendBelowHeight = 15000.0;
```

- [ ] **Step 5: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Gear
```
Expected: `8 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp
git commit -m "feat(entities): the 737 declares its gear cycle and its cue heights"
```

---

### Task 5: Surface it to Animation Blueprints

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideAgentAnim.h` — four `BlueprintReadOnly` outputs after `bAirborne` (line 113), two `EditDefaultsOnly` rig facts after `MainWheelRadius`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideAgentAnim.cpp` — in `NativeUpdateAnimation`, after `bAirborne = Motion.bAirborne;`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp` (extend; add `#include "Present/AirsideAgentAnim.h"`)

**Interfaces:**
- Consumes: `FAgentMotion::GearDownFraction`, `FAgentMotion::BayDoorOpenFraction` from Task 3.
- Produces: `UAirsideAgentAnim::GearDownFraction`, `BayDoorOpenFraction`, `GearAngleDegrees`, `BayDoorAngleDegrees` (all `float`, `BlueprintReadOnly`); `GearRetractedAngleDegrees`, `BayDoorOpenAngleDegrees` (`float`, `EditDefaultsOnly`); `static void GearAnglesFrom(float GearDownFraction, float DoorOpenFraction, float RetractedAngle, float DoorAngle, float& OutGearAngle, float& OutDoorAngle)`.

- [ ] **Step 1: Write the failing test**

Append to `GearCycleTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearAnglesFollowTheFractionsTest,
	"Airside.Present.GearAnglesFollowTheFractions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearAnglesFollowTheFractionsTest::RunTest(const FString& Parameters)
{
	// STATIC AND FREE OF THE INSTANCE, the same construction PropStepDegrees and
	// WheelStepDegrees use, so the arithmetic can be tested with no actor and no skeleton.
	float GearAngle = -1.0f;
	float DoorAngle = -1.0f;

	// plane4's measured rig angles: gear folds 90 degrees, the nose doors sweep 81 - "found
	// by sweeping: the two free edges meet on the centreline to 0.0 mm", per its
	// build_export.py. Not invented here and not typed into a Blueprint.
	const float Retracted = 90.0f;
	const float Door = 81.0f;

	// DOWN AND LOCKED IS ZERO ROTATION. The bind pose IS the gear-down pose, so a bone driven
	// to anything but zero here would sit an aeroplane on a leg it has already folded.
	UAirsideAgentAnim::GearAnglesFrom(1.0f, 0.0f, Retracted, Door, GearAngle, DoorAngle);
	TestEqual(TEXT("gear down is no rotation at all"), GearAngle, 0.0f);
	TestEqual(TEXT("and a shut door is no rotation either"), DoorAngle, 0.0f);

	// FULLY STOWED IS THE WHOLE TRAVEL.
	UAirsideAgentAnim::GearAnglesFrom(0.0f, 1.0f, Retracted, Door, GearAngle, DoorAngle);
	TestEqual(TEXT("gear up is the full fold"), GearAngle, 90.0f);
	TestEqual(TEXT("and an open bay is the full sweep"), DoorAngle, 81.0f);

	// AND IT IS A TRAVEL, NOT A SWITCH - the mid-cycle value, which is the only one a
	// two-pose implementation could not produce.
	UAirsideAgentAnim::GearAnglesFrom(0.5f, 0.5f, Retracted, Door, GearAngle, DoorAngle);
	TestEqual(TEXT("half retracted is half the fold"), GearAngle, 45.0f);
	TestEqual(TEXT("half open is half the sweep"), DoorAngle, 40.5f);

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run the build line.
Expected: FAILS to compile — `'GearAnglesFrom': is not a member of 'UAirsideAgentAnim'`.

- [ ] **Step 3: Add the properties and the static helper**

In `AirsideAgentAnim.h`, after the `bAirborne` property:

```cpp
	/**
	 * Where the gear is: 1 down and locked, 0 stowed. Copied from the model, not derived.
	 *
	 * THE MODEL OWNS THE CYCLE - see FGearPerformance and FRoadAgent::AdvanceGear. This class
	 * once derived the propeller's speed from a running flag, "which made the propeller a
	 * switch", and that was moved into the model for exactly the reason that applies here
	 * with more force: a gear cycle run from an anim instance is a switch between two poses
	 * instead of a travel.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float GearDownFraction = 1.0f;

	/** The gear bay doors: 0 shut, 1 fully open. Copied from the model. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float BayDoorOpenFraction = 0.0f;

	/**
	 * Gear rotation, degrees. Apply to gear_nose, gear_L and gear_R - the RETRACT bones, not
	 * the rolling ones, which take WheelAngleDegrees.
	 *
	 * ZERO IS DOWN AND LOCKED, because the bind pose is the gear-down pose. Every bone in
	 * this rig rotates about its own LENGTH, so the graph applies this in Bone Space and
	 * never argues about world axes.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float GearAngleDegrees = 0.0f;

	/** Bay door rotation, degrees. Apply to door_nose_L and door_nose_R. Zero is shut. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float BayDoorAngleDegrees = 0.0f;
```

After `MainWheelRadius`:

```cpp
	/**
	 * How far this rig's gear folds, degrees. plane4's is 90.
	 *
	 * ON THE ANIM INSTANCE BECAUSE IT IS A FACT ABOUT ONE RIG, exactly as MainWheelRadius is,
	 * and MEASURED rather than chosen - plane4/scripts/build_export.py poses the leg to find
	 * it. The model publishes a fraction and knows nothing about this number; a travel angle
	 * in Model/ would be a rig detail in a layer that has no rigs.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside")
	float GearRetractedAngleDegrees = 90.0f;

	/**
	 * How far this rig's bay doors sweep, degrees. plane4's is 81 - "found by sweeping: the
	 * two free edges meet on the centreline to 0.0 mm", which is a measurement and not a
	 * round number, and is why it is not simply 90.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside")
	float BayDoorOpenAngleDegrees = 81.0f;
```

And beside the other statics, after `WheelStepDegrees`'s declaration:

```cpp
	/**
	 * Both bone angles from both fractions. Zero is the bind pose for each.
	 *
	 * Static and free of the instance so it can be tested without an actor or a skeleton -
	 * the same reason PropStepDegrees and WheelStepDegrees are.
	 */
	static void GearAnglesFrom(float GearDownFraction, float DoorOpenFraction,
		float RetractedAngle, float DoorAngle, float& OutGearAngle, float& OutDoorAngle);
```

- [ ] **Step 4: Implement and wire it**

In `AirsideAgentAnim.cpp`, after `bAirborne = Motion.bAirborne;`:

```cpp
	// THE GEAR, COPIED AND NOT DERIVED - the model ran the cycle, doors and all, and this is
	// where it got to. See FRoadAgent::AdvanceGear.
	GearDownFraction = static_cast<float>(Motion.GearDownFraction);
	BayDoorOpenFraction = static_cast<float>(Motion.BayDoorOpenFraction);
	GearAnglesFrom(GearDownFraction, BayDoorOpenFraction, GearRetractedAngleDegrees,
		BayDoorOpenAngleDegrees, GearAngleDegrees, BayDoorAngleDegrees);
```

At the end of the file:

```cpp
void UAirsideAgentAnim::GearAnglesFrom(float GearDownFraction, float DoorOpenFraction,
	float RetractedAngle, float DoorAngle, float& OutGearAngle, float& OutDoorAngle)
{
	// ONE MINUS THE FRACTION, because the fraction counts DOWNNESS and the angle counts
	// travel away from the bind pose. Getting this the other way round parks an aeroplane on
	// a folded leg, which is a state the mesh can express perfectly happily.
	OutGearAngle = (1.0f - GearDownFraction) * RetractedAngle;
	OutDoorAngle = DoorOpenFraction * DoorAngle;
}
```

- [ ] **Step 5: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Gear
./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.Gear
```
Expected: `8 test(s) run, 0 failed, 0 crashed` and `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Run the whole suite**

```
./Tools/Run-AirsideTests.ps1
```
Expected: no failures and no crashes. `FAgentMotion` gained members and `FRoadAgent` gained a per-frame call, so this is the run that catches anything that assumed either.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/AirsideAgentAnim.h Plugins/Airside/Source/Airside/Private/Present/AirsideAgentAnim.cpp Plugins/Airside/Source/AirsideTests/Private/GearCycleTest.cpp
git commit -m "feat(present): gear and bay door angles, ready for an animgraph"
```

---

### Task 6: Teach the bone plan the two new variables

**Files:**
- Modify: `Tools/Python/build_plane2_anim.py` — `bone_plan()`
- Modify: `C:\repos\AirportMgr2Models\plane4\scripts\build_export.py` — the docstring's "NO NEW BONE MAY CONTAIN" paragraph (separate repo; commit there separately)

**Interfaces:**
- Consumes: the property names produced by Task 5 — `GearAngleDegrees`, `BayDoorAngleDegrees`.
- Produces: nothing code depends on. This is the list that must agree with Task 5's property names.

- [ ] **Step 1: Add the two rules**

In `bone_plan()`, replace the `elif "wheel" in lowered:` branch and the `else:` that follows it with:

```python
        elif "wheel" in lowered:
            plan.append((name, "WheelAngleDegrees"))
        elif "gear" in lowered:
            # AFTER the wheel rule, and the ordering is the discipline rather than a
            # necessity today: no bone in plane4's rig matches two of these five. The steer
            # rule above records what happens when one does - 'nosewheel_steer' matches both
            # 'steer' and 'wheel', and the wrong winner spins the nose gear like a castor.
            # Most-specific-first is what keeps the next rig from discovering that again.
            plan.append((name, "GearAngleDegrees"))
        elif "door" in lowered:
            plan.append((name, "BayDoorAngleDegrees"))
        else:
            plan.append((name, "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"))
```

And extend the function's docstring:

```python
    """(bone, variable) per joint the export declares, root excluded.

    The NAMES come from the .glb; the MAPPING is the decision this script owns. A joint that
    matches neither rule is reported rather than skipped silently - an unrecognised bone is
    either a rig the sim does not know how to drive yet, or a typo, and both want saying.

    THIS LIST MUST AGREE WITH UAirsideAgentAnim'S PROPERTY NAMES and there is no compiler to
    check it - see CLAUDE.md, "check where a list is CONSUMED". gear/door were added on
    2026-09-19 with the retraction work; before that plane4's five retract and hinge bones
    were correctly reported UNRECOGNISED, which its own build_export.py predicted in writing.
    """
```

- [ ] **Step 2: Verify the plan reports the right variables**

The editor must be closed. Run:

```
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_plane2_anim.py" -unattended -nosplash -nopause
```

Then read the markers:
```
grep -a "MARKER:" Saved/Logs/AirportMgr.log | sed 's/.*MARKER: //'
```

Expected: plane2's seven joints map as before — `prop_L`/`prop_R` to `PropAngleDegrees`, `nosewheel_steer` to `SteerAngleDegrees`, `nosewheel`/`wheel_L`/`wheel_R` to `WheelAngleDegrees` — and **nothing is UNRECOGNISED**. This script reads plane2's `.glb`, so it does not exercise the new rules; it is run to prove the edit broke nothing that worked.

- [ ] **Step 3: Check the new rules against plane4's rig names directly**

plane4 has no `build_plane4_anim.py` (creating one, and `ABP_Plane4`, is out of scope — see below). Verify the matcher against its joint names with a bare Python check:

```bash
python -c "
names = ['root','door_nose_L','door_nose_R','gear_L','wheel_L','gear_nose','nosewheel_steer','nosewheel','gear_R','wheel_R','prop_L','prop_R']
for n in names:
    low = n.lower()
    if low == 'root': v = '(skipped)'
    elif 'prop' in low: v = 'PropAngleDegrees'
    elif 'steer' in low: v = 'SteerAngleDegrees'
    elif 'wheel' in low: v = 'WheelAngleDegrees'
    elif 'gear' in low: v = 'GearAngleDegrees'
    elif 'door' in low: v = 'BayDoorAngleDegrees'
    else: v = 'UNRECOGNISED'
    print('%-18s %s' % (n, v))
"
```

Expected exactly:
```
root               (skipped)
door_nose_L        BayDoorAngleDegrees
door_nose_R        BayDoorAngleDegrees
gear_L             GearAngleDegrees
wheel_L            WheelAngleDegrees
gear_nose          GearAngleDegrees
nosewheel_steer    SteerAngleDegrees
nosewheel          WheelAngleDegrees
gear_R             GearAngleDegrees
wheel_R            WheelAngleDegrees
prop_L             PropAngleDegrees
prop_R             PropAngleDegrees
```

Nothing UNRECOGNISED, and `nosewheel` still takes `WheelAngleDegrees` rather than being captured by a new rule.

- [ ] **Step 4: Update the export script's warning**

In `C:\repos\AirportMgr2Models\plane4\scripts\build_export.py`, replace the paragraph beginning `NO NEW BONE MAY CONTAIN "wheel", "steer" OR "prop".` with:

```
NO NEW BONE MAY CONTAIN "wheel", "steer", "prop", "gear" OR "door". That plan
matches by SUBSTRING, so a retract bone called `nosewheel_retract` would be
wired to WheelAngleDegrees and the whole nose leg would spin about its own axle
like a castor. Hence gear_nose rather than nosewheel_retract.

"gear" and "door" joined the list on 2026-09-19, when AirportMgr2 gained
GearAngleDegrees and BayDoorAngleDegrees - so the five bones this file used to
predict would be reported UNRECOGNISED are now driven. The prediction below is
superseded: retraction IS driven, by FRoadAgent::AdvanceGear.
```

And replace the `RETRACTION IS RIGGED BUT NOT YET DRIVEN.` paragraph with:

```
RETRACTION IS RIGGED AND NOW DRIVEN. The engine gained a gear cycle on
2026-09-19 - FGearPerformance, FRoadAgent::AdvanceGear, and GearAngleDegrees /
BayDoorAngleDegrees on UAirsideAgentAnim - so gear_nose, gear_L, gear_R,
door_nose_L and door_nose_R all have a variable behind them. The doors are
sequenced around the gear travel: open, travel, close.
```

- [ ] **Step 5: Commit both repos**

```bash
git add Tools/Python/build_plane2_anim.py
git commit -m "fix(tools): the bone plan knows gear and door bones"
```

The models repo is separate; commit `build_export.py` there with the same message body.

---

## Out of scope, and deliberately

- **`ABP_Plane4` and its animgraph.** UE 5.8 exposes no Python for creating or connecting Blueprint graph nodes, so the Transform (Modify) Bone nodes are a manual editor job against the bone names Task 6 prints. Nothing in this plan needs them to be tested. **Corrected 2026-09-20:** the engine does expose it, through `UBlueprintGraphEditor` and MCP - so this is now scriptable work, not a manual job. `docs/2026-09-20-animgraph-authoring.md`.
- **`DA_Aircraft_Plane4`.** `Build737` and `DA_Aircraft_B738` already exist and are where the 737's figures live; wiring plane4's mesh to that type is separate work.
- **Gear as drag.** Nothing in the climb or approach reads `GearDownFraction`.
- **Reversing a cycle mid-travel.** Commented at the site in Task 2.

---

## Self-review notes

Checked against the spec, section by section:

- §1 (height not lift-off, `bAirborne` as precondition) — Task 2 Step 1, `FGearRetractsAtHeightNotLiftOff`.
- §1 (extension built but latent) — Task 1 `FGearExtendMirrorsRetract`, Task 4's `ExtendBelowHeight`.
- §2 (four phases, one timer, one evaluator) — Task 1 Steps 3 and 4.
- §3 (`FGearPerformance` on `FAirframe`, zero means fixed) — Task 1 Step 3 plus the `FAirframe` member noted in Task 4; `FGearFixedWhenUnauthored` covers the zero case.
- §3 (heights read by phase) — Task 2, `FGearDescendingArrivalDoesNotRetract`.
- §4 (`FAgentMotion` fields, derived angles, measured rig facts) — Tasks 3 and 5.
- §5 (bone matcher) — Task 6.
- §6 (plane4 only) — Task 4, asserted both ways: the 737 is set, the Meridian is not.
- §7 (all five test cases) — Tasks 1–5; each pins a value that must change as well as one that must not.
- §8 (full build, no Live Coding) — Global Constraints.

**Two defects found and fixed during this review**, recorded because both are the kind that
would have cost an executor a build each:

- Task 2 reads `Airframe.Gear`, so the `FAirframe` member has to exist by then. It was
  originally described in Task 4's preamble, two tasks after it is first needed; it now
  belongs to Task 1 Step 3, beside the struct it carries.
- Task 3 Step 4 named a function that does not exist and then told the reader to delete it.
  Replaced with the single real line, `GearFractions(Motion.GearDownFraction,
  Motion.BayDoorOpenFraction);`.

**Access checked, not assumed:** `FRoadAgent` is a `struct`, and `Phase`, `LastMotion` and
`EngineRPM` all sit above the `private:` at line 491 — so the tests in Tasks 2 and 3 can set
those members directly, which is what every case in this plan depends on.
