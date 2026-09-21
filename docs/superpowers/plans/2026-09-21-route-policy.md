# Route Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace ten hand-assembled routing policies with one table keyed by a named errand, so an aircraft can no longer taxi down a runway because a call site forgot to say it shouldn't.

**Architecture:** A new `Model/RoutePolicy.h` holds `ERouteErrand` and `FRoutePolicy::For()` — the single table. `FRouteQuery` carries the errand and its resolved policy; `RouteSearch` refuses a query with no errand and one whose occupancy pointer disagrees with its policy. Runway avoidance stays a hard filter for errands that must never use a strip, with a multiplicative penalty underneath for the rest, so a forgotten errand degrades to a detour rather than a taxi down the runway.

**Tech Stack:** UE 5.8.2 C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST` automation tests, PowerShell for the architecture lint.

**Spec:** `docs/superpowers/specs/2026-09-21-route-policy-design.md`

## Global Constraints

- **Branch:** `feature/route-policy`, already created, spec already committed at `2f913a5`.
- **The editor must be CLOSED for every build in this plan.** Tasks 1, 3, 4 and 6 add `UENUM`s, `USTRUCT`s and `UPROPERTY`s; Live Coding covers function bodies only. Alternatively work in a git worktree and add `-NoHotReloadFromIDE` — never pass that flag on `C:\repos\AirportMgr2` itself.
- **Build:** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`
- **Tests:** `./Tools/Run-AirsideTests.ps1 -Filter <name>`. **Never trust the exit code** — read the `N test(s) run, N failed, N crashed` line.
- **A new test .cpp needs two builds.** The first reports `Succeeded` without compiling it. Any task that creates a test file builds twice before believing a pass.
- **Test names must be distinct leaves.** UE's automation tree silently drops a bare-named test once a dotted child exists. Never register a test at `Airside.Model.RoutePolicy`; always a leaf below it.
- **Log category:** `LogAirside` (from `AirsideLog.h`) for the plugin. Refusals log at `Error`, never `Warning` — warnings do not fail automation tests in this project.
- **Every `UE_LOG` and every WHY comment survives a move.** Count `UE_LOG(` before and after in touched files.
- Do not add a `Co-Authored-By` trailer beyond the one shown in the commit commands below.

---

### Task 1: `FRoutePolicy` — the table, standalone

Creates the policy type and its table with nothing wired to it yet. It compiles, it is tested, and nothing else in the codebase changes.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/RoutePolicy.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/RoutePolicy.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp`

**Interfaces:**
- Consumes: `ERunwayAvoidance` from `Model/RouteSearch.h`.
- Produces: `ERouteErrand` (12 enumerators), `EOccupancyUse { Never, Required }`, `FRoutePolicy { ERunwayAvoidance Avoidance; EOccupancyUse Occupancy; bool bPenaliseRunways; }`, and `FRoutePolicy FRoutePolicy::For(ERouteErrand)`.

> **Header placement:** `ERunwayAvoidance` lives in `RouteSearch.h` today and `RoutePolicy.h` must include it. That is the right direction — the policy is a statement *about* the search's existing vocabulary — and it keeps this task from touching `RouteSearch.h` at all. Do not move `ERunwayAvoidance`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoutePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

// A LEAF NAME, never the bare "Airside.Model.RoutePolicy": UE's automation tree drops a
// bare-named test the moment a dotted child is registered under it, silently and with no
// error, so the sibling below would have deleted this one from the run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoutePolicyTableTest,
	"Airside.Model.RoutePolicy.EveryErrandHasARow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoutePolicyTableTest::RunTest(const FString& Parameters)
{
	// BY REFLECTION, not a hand-written list of errands: a hand-written list is a second
	// list that must agree with the enum, and CLAUDE.md's rule is that lists which must
	// agree are one list. An errand added without a table row is exactly the failure this
	// whole design exists to stop, so the test must find it without being told it exists.
	const UEnum* Enum = StaticEnum<ERouteErrand>();
	if (!TestNotNull(TEXT("ERouteErrand is a reflected UENUM, or For() cannot be swept"), Enum))
	{
		return false;
	}

	// NumEnums() counts the compiler-generated _MAX sentinel, so stop one short of it.
	for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
	{
		const ERouteErrand Errand = static_cast<ERouteErrand>(Enum->GetValueByIndex(Index));
		const FString Name = Enum->GetNameStringByIndex(Index);
		const FRoutePolicy Policy = FRoutePolicy::For(Errand);

		if (Errand == ERouteErrand::Unset)
		{
			// Unset is the one errand with no meaningful row. It must still be answerable -
			// For() is called before the search's refusal, not after - and the safest thing
			// to answer with is the most restrictive row, so a policy read through a path
			// that skipped the refusal still cannot put an aircraft on a strip.
			TestEqual(TEXT("Unset resolves to the most restrictive avoidance"),
				Policy.Avoidance, ERunwayAvoidance::All);
			continue;
		}

		// GraphProbe is the ONLY errand allowed to be fully permissive. Every other row
		// must either keep aircraft off a strip or charge them for it; a row that does
		// neither is a row somebody forgot to fill in, which reads identically to the bug.
		if (Errand != ERouteErrand::GraphProbe && Errand != ERouteErrand::DepartureBacktrack)
		{
			const bool bConstrained =
				Policy.Avoidance != ERunwayAvoidance::None || Policy.bPenaliseRunways;
			TestTrue(*FString::Printf(
				TEXT("%s either avoids runways or pays for them; a row that does neither is unfilled"),
				*Name), bConstrained);
		}

		// A penalty under an All filter can never be reached - the edge is gone before the
		// cost is asked for - so a row carrying both is stating something no code path can
		// observe, and the next reader would believe a penalty was being applied.
		if (Policy.Avoidance == ERunwayAvoidance::All)
		{
			TestFalse(*FString::Printf(
				TEXT("%s: a penalty under an All filter is unreachable and must not be claimed"),
				*Name), Policy.bPenaliseRunways);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoutePolicyCallSitesTest,
	"Airside.Model.RoutePolicy.MatchesCallSitesAsShipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoutePolicyCallSitesTest::RunTest(const FString& Parameters)
{
	// THE REFACTOR CONTRACT, pinned. Each row below is what that call site set by hand on
	// 2026-09-21, before the table existed. This test fails if the migration quietly
	// changed a rule it promised not to - which is the only way a "no behaviour change"
	// claim can be measured rather than asserted.
	//
	// The five sites the spec says DO change behaviour are deliberately absent: they had no
	// policy to preserve. See spec section 7.
	auto Row = [this](ERouteErrand Errand, ERunwayAvoidance Avoidance, EOccupancyUse Occupancy, const TCHAR* Site)
	{
		const FRoutePolicy Policy = FRoutePolicy::For(Errand);
		TestEqual(*FString::Printf(TEXT("%s kept its runway avoidance"), Site), Policy.Avoidance, Avoidance);
		TestEqual(*FString::Printf(TEXT("%s kept its occupancy use"), Site), Policy.Occupancy, Occupancy);
	};

	Row(ERouteErrand::ArrivalTaxiIn,      ERunwayAvoidance::All,  EOccupancyUse::Never,    TEXT("ArrivalPlanner.cpp:36"));
	Row(ERouteErrand::DepartureToEntry,   ERunwayAvoidance::All,  EOccupancyUse::Never,    TEXT("DeparturePlanner.cpp:88"));
	Row(ERouteErrand::DepartureBacktrack, ERunwayAvoidance::None, EOccupancyUse::Never,    TEXT("DeparturePlanner.cpp:106"));
	Row(ERouteErrand::Replan,             ERunwayAvoidance::Held, EOccupancyUse::Required, TEXT("GroundTrafficRebuild.cpp:95"));
	Row(ERouteErrand::CandidateComparison, ERunwayAvoidance::All, EOccupancyUse::Never,    TEXT("FuelService.cpp:234"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoutePolicy`

Expected: the **build** fails with `Cannot open include file: 'Model/RoutePolicy.h'`. That is the correct first failure — the header does not exist yet.

- [ ] **Step 3: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Model/RoutePolicy.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "RoutePolicy.generated.h"

/**
 * What a route is FOR. The single axis every routing policy is derived from.
 *
 * NOT WHO IS DRIVING, and the distinction is the whole reason this enum is allowed to exist
 * beside ETraversalClass, whose own comment forbids the neighbouring design: "what they do
 * is a service role... deliberately not this enum - otherwise this grows with every vehicle
 * type in the game and gets consulted by pathfinding for no reason". That warning is about
 * the ACTOR. This names the ERRAND, which is the one thing pathfinding genuinely must
 * consult, because it is the question the policy answers.
 *
 * THE TEST THAT KEEPS THEM APART: adding a vehicle type must not add an errand. A catering
 * truck and a fuel truck are both VehicleToJob.
 */
UENUM()
enum class ERouteErrand : uint8
{
	/**
	 * Never valid. RouteSearch::Find and FindToGoals refuse it and log at Error.
	 *
	 * ZERO, AND THE DEFAULT, deliberately: the bug this whole design exists to delete was
	 * four call sites that never mentioned runway avoidance and silently got the permissive
	 * answer. A default that cannot be searched with turns forgetting from a silent taxi
	 * down a runway into a line in the log at the first repro.
	 */
	Unset = 0,

	/** An arrival from a runway exit to a stand. Never touches a strip. */
	ArrivalTaxiIn,

	/** A departure from a stand to its runway entry. Never touches a strip. */
	DepartureToEntry,

	/**
	 * A departure backtracking up the strip to a threshold. THE ONE ERRAND THAT MUST USE A
	 * RUNWAY, and the reason the ban could never simply be made unconditional.
	 */
	DepartureBacktrack,

	/** Pushback's clearance route, off the stand and clear of the departure's own arm. */
	PushbackClear,

	/** The taxi from where a push ends to the runway entry. */
	PushbackTaxiOut,

	/**
	 * A deadlock or failed-edge replan for an agent already under way. May use a runway end
	 * that is FREE - an agent turning round via a free runway end is what the player expects
	 * to see, and the outright ban that preceded Held sent one round the whole taxiway loop
	 * past an end it could have used.
	 */
	Replan,

	/** A graph rebuild re-resolving a surviving plan onto new handles. */
	RebuildReResolve,

	/** A vehicle driving to or from a job. Re-chosen on dispatch, so it takes the table. */
	VehicleToJob,

	/**
	 * Comparing candidates - which depot, which stand. Shape only.
	 *
	 * A congestion-weighted comparison makes the WINNER flicker between ticks without ever
	 * changing what gets driven, which is what FuelService.cpp's own comment refused.
	 */
	CandidateComparison,

	/** A route the player or a tool asked for directly. */
	PlayerIssued,

	/**
	 * No policy: plain shortest path, runways free, no congestion. TESTS AND TOOLS ONLY.
	 *
	 * It exists so a graph-shape test ("is B reachable from A") need not pick a production
	 * errand whose policy it does not care about and would silently inherit. Without it,
	 * forty existing tests would each be asserting something about routing policy by
	 * accident.
	 *
	 * Check-Architecture.ps1 rule 11 fails the build if this name appears outside a test
	 * module or a Tool/ directory: the permissive default survives only where it is NAMED.
	 */
	GraphProbe,
};

/**
 * Whether an errand's cost reads the occupancy table.
 *
 * NOT "vehicles yes, aircraft no", which is what the ground-traffic spec's section 4 says and
 * has not been true since FuelService landed. The axis that actually survives all ten call
 * sites is whether THE ROUTE MAY STILL CHANGE: a route fixed when it is issued costs shape
 * only, a route being re-chosen for an agent already under way costs held length too.
 */
UENUM()
enum class EOccupancyUse : uint8
{
	/**
	 * Cost is shape only. A clearance, or a comparison that must not flicker.
	 *
	 * The search REFUSES a query that supplied a table anyway - see RouteSearch::Find. A
	 * caller that went to the trouble of passing occupancy believes it is being weighted by
	 * it, and silently ignoring the pointer leaves that caller reasoning about a cost term
	 * the search never applied.
	 */
	Never,

	/** Cost includes held length. The search REFUSES if no table was supplied. */
	Required,
};

/**
 * One errand's routing policy. THE one table - see FRoutePolicy::For.
 *
 * A NAMED PUBLIC TYPE rather than a constexpr array hidden in RouteSearch.cpp, so the table
 * can be swept by a test (Airside.Model.RoutePolicy.EveryErrandHasARow) and pinned against
 * what the call sites shipped (MatchesCallSitesAsShipped). A table nothing can enumerate is
 * a fourth outing for the bug in CLAUDE.md's "Check where a list is CONSUMED".
 */
USTRUCT()
struct AIRSIDE_API FRoutePolicy
{
	GENERATED_BODY()

	/** Which runway-derived edges the search may not use at all. */
	UPROPERTY() ERunwayAvoidance Avoidance = ERunwayAvoidance::All;

	/** Whether the cost reads the occupancy table, checked BOTH WAYS by the search. */
	UPROPERTY() EOccupancyUse Occupancy = EOccupancyUse::Never;

	/**
	 * Whether a runway edge this policy still ALLOWS costs a multiple of its length.
	 *
	 * A FLAG, NOT THE NUMBER. The magnitude is FTrafficRules::RunwayPenalty, copied onto
	 * FRouteQuery::RunwayPenalty, so it reaches the Details panel and can be tuned on a
	 * placed actor without a rebuild. The policy says WHETHER, the rules say HOW MUCH.
	 *
	 * Meaningless - and asserted false by the table test - when Avoidance is All: the edge
	 * is gone before anything asks its cost.
	 */
	UPROPERTY() bool bPenaliseRunways = false;

	/**
	 * The table. Every errand has exactly one row.
	 *
	 * DEFAULTS ARE THE RESTRICTIVE ONES on this struct, so an errand added without a row
	 * here gets All/Never rather than the permissive answer - and the table test catches it
	 * on the next run either way.
	 */
	static FRoutePolicy For(ERouteErrand Errand);
};
```

- [ ] **Step 4: Write the table**

Create `Plugins/Airside/Source/Airside/Private/Model/RoutePolicy.cpp`:

```cpp
#include "Model/RoutePolicy.h"

FRoutePolicy FRoutePolicy::For(ERouteErrand Errand)
{
	auto Make = [](ERunwayAvoidance Avoidance, EOccupancyUse Occupancy, bool bPenalise)
	{
		FRoutePolicy Policy;
		Policy.Avoidance = Avoidance;
		Policy.Occupancy = Occupancy;
		Policy.bPenaliseRunways = bPenalise;
		return Policy;
	};

	switch (Errand)
	{
	// A SWITCH WITH NO DEFAULT, deliberately: adding an enumerator without a row here is a
	// compiler warning at this switch, which is a cheaper place to find out than the table
	// test and very much cheaper than play.
	case ERouteErrand::ArrivalTaxiIn:
		// As shipped. The query carries no occupancy - ChooseStand reads the table
		// separately, to skip a stand somebody is on, never to weight an edge. A stand
		// chosen by congestion would be re-chosen every tick until the aircraft committed.
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::DepartureToEntry:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::DepartureBacktrack:
		// NO PENALTY. The backtrack's whole purpose is the strip; charging it would make a
		// legal backtrack lose to nothing at all, since there is no alternative to lose to.
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, false);

	case ERouteErrand::PushbackClear:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::PushbackTaxiOut:
		// GAINS All. This site never declared a policy and got None by omission - it is one
		// of the two that reproduce the 2026-09-21 report, being the long taxi from where a
		// push ends to the runway entry.
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::Replan:
		return Make(ERunwayAvoidance::Held, EOccupancyUse::Required, true);

	case ERouteErrand::RebuildReResolve:
		// GAINS Held. ReResolvePlan set congestion but never avoidance.
		return Make(ERunwayAvoidance::Held, EOccupancyUse::Required, true);

	case ERouteErrand::VehicleToJob:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Required, false);

	case ERouteErrand::CandidateComparison:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::PlayerIssued:
		// NONE, NOT All, and this is the one row where graceful degradation does the work: a
		// player who clicks two points across a runway is stating an intent, and refusing it
		// outright reads as a broken tool. The penalty sends the route round when round
		// exists and takes the strip when it does not.
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, true);

	case ERouteErrand::GraphProbe:
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, false);

	case ERouteErrand::Unset:
		break;
	}

	// Unset, and anything a future cast smuggles past the switch. The MOST RESTRICTIVE row,
	// not the permissive one: this is reached before the search's own refusal has had a
	// chance to fire, and a policy read through some path that skipped that refusal still
	// must not put an aircraft on a strip.
	return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);
}
```

- [ ] **Step 5: Build twice, then run the test**

A new test .cpp needs two builds — the first reports `Succeeded` without compiling it.

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoutePolicy
```

Expected: the summary line reads `2 test(s) run, 0 failed, 0 crashed`. **Two**, not one — if it says 1, the bare-parent collision has eaten a test and the names need re-checking.

- [ ] **Step 6: Prove the table test measures something**

Temporarily change `PushbackTaxiOut`'s row to `Make(ERunwayAvoidance::None, EOccupancyUse::Never, false)` and re-run. Expected: `EveryErrandHasARow` FAILS with "PushbackTaxiOut either avoids runways or pays for them". Revert the change and re-run to green.

A green test that cannot go red is measuring nothing; this step is the proof it can.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoutePolicy.h \
        Plugins/Airside/Source/Airside/Private/Model/RoutePolicy.cpp \
        Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp
git commit -m "feat(routing): FRoutePolicy, one table keyed by errand

Nothing consults it yet. Two tests: every errand has a row, and the six
rows whose call sites already declared a policy match what shipped.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Memoise "is this edge on a runway", per search

A pure structural change with no behaviour change, done before the penalty needs it. `ExpandNode` already calls `Network.IsRunwaySegment` on every relaxation of every edge whenever avoidance is on — a slot lookup plus a profile resolve, which is exactly the per-relaxation cost `FGuidelineEdge::Length` was cached to remove in #171. The penalty in Task 4 would ask the same question on every edge in every search, so it gets a memo first.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp:80-144` (`ExpandNode`), `:310` and `:555` (the two callers), `:273-355` (`RunSearch`), `:522-600` (`FindToGoals`)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h` (two counter declarations, beside `NodeVisitCountForTest`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp` (add one test)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `RouteSearch::RunwaySeedResolveCountForTest()` and `RouteSearch::ResetRunwaySeedResolveCountForTest()`, both `AIRSIDE_API int32`/`void`. `ExpandNode` gains a `TMap<int32, bool>& RunwaySeeds` parameter immediately after the existing `TMap<int32, bool>& RunwayInUse`.

> **Why a per-search memo and not a cached UPROPERTY on `FGuidelineEdge`:** `Length` is invalidated by the three writers that can move an edge's endpoints or `Control`, and that set is closed. Runway-ness depends on the *profile*, which changes when a road is re-profiled — a different and open invalidation trigger, on serialised data, with no writer positioned to catch it. A stale `true` would refuse taxiways for the rest of the session. The memo has no invalidation problem because it does not outlive the search.

- [ ] **Step 1: Write the failing test**

Append to `Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwaySeedMemoTest,
	"Airside.Model.RouteSearch.RunwaySeedMemo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwaySeedMemoTest::RunTest(const FString& Parameters)
{
	// A LADDER ALONG ONE STRIP: five guideline edges all DerivedFrom the SAME runway
	// segment, so a search that walks them all asks "is this a runway" five times and must
	// resolve the seed exactly once.
	//
	// MEASURES THE MEMO, does not name it. Without the counter this would be an
	// optimisation nobody could prove was wired - the search returns the same route either
	// way, which is precisely why a correctness assertion here would be green on a memo
	// that had been deleted.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadA = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadB = Net->AddNode(FVector2D(50000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadA, RoadB, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	TArray<FGuidelineNodeId> Chain;
	for (int32 Index = 0; Index <= 5; ++Index)
	{
		Chain.Add(Net->AddGuidelineNode(FVector2D(Index * 10000.0, -1000.0)));
	}
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FGuidelineEdge Along;
		Along.A = Chain[Index];
		Along.B = Chain[Index + 1];
		Along.Control = FVector2D((Index * 10000.0) + 5000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	RouteSearch::ResetRunwaySeedResolveCountForTest();

	FRouteQuery Q;
	Q.Start = Chain[0];
	Q.Goal = Chain.Last();
	Q.Class = ETraversalClass::Aircraft;
	// Held, not All: All would delete every edge and the search would never reach the cost,
	// so the ladder would be walked once and the memo would look unnecessary.
	Q.AvoidRunways = ERunwayAvoidance::Held;

	const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
	TestTrue(TEXT("the ladder is routable, or the count below measures an empty search"), Plan.IsValid());

	TestEqual(TEXT("one seed resolved once, however many of its edges the search relaxed"),
		RouteSearch::RunwaySeedResolveCountForTest(), 1);

	return true;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RouteSearch.RunwaySeedMemo`

Expected: the build fails with `'RunwaySeedResolveCountForTest': is not a member of 'RouteSearch'`.

- [ ] **Step 3: Declare the counters**

In `Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h`, immediately after `ResetNodeVisitCountForTest`'s declaration and its comment:

```cpp
	/**
	 * How many times a runway SEED has been resolved through URoadNetwork::IsRunwaySegment
	 * inside a search, since the last reset - the measurement that ExpandNode's per-search
	 * memo is actually consulted, rather than that it merely exists.
	 *
	 * A seed, not an edge: a long strip carries many guideline edges and one segment, and
	 * the whole point of the memo is that the second edge along it costs nothing. See
	 * Airside.Model.RouteSearch.RunwaySeedMemo.
	 */
	AIRSIDE_API int32 RunwaySeedResolveCountForTest();

	/** Zeroes the counter above, so an earlier search's resolves are never mistaken for the
	 *  ones a test is about to measure. */
	AIRSIDE_API void ResetRunwaySeedResolveCountForTest();
```

- [ ] **Step 4: Add the memo and the counter**

In `RouteSearch.cpp`, beside `GSearchCallCountForTest` in the anonymous namespace:

```cpp
	/** See RouteSearch::RunwaySeedResolveCountForTest. Bumped once per seed actually
	 *  resolved - a memo hit does not count, which is what makes the count measure the
	 *  memo rather than the traffic through it. */
	int32 GRunwaySeedResolveCountForTest = 0;
```

In `ExpandNode`, change the signature to add the new map after `RunwayInUse`:

```cpp
	void ExpandNode(const URoadNetwork& Network, const FRouteQuery& Query, bool bIgnoreWingspan,
		FGuidelineNodeId At, double Reached, const TSet<FGuidelineNodeId>& Closed,
		TFunctionRef<double(const FVector2D&)> Heuristic, TMap<int32, bool>& RunwayInUse,
		TMap<int32, bool>& RunwaySeeds,
		TMap<FGuidelineNodeId, double>& Best, TMap<FGuidelineNodeId, FRouteStep>& Arrived,
		TArray<TPair<double, FGuidelineNodeId>>& Open)
```

Immediately before the existing `IsRunwayHeld` lambda, add its sibling:

```cpp
		// Whether an edge's source segment IS a runway, once per segment seen. A slot lookup
		// plus a profile resolve, which the avoidance test below used to pay on EVERY
		// relaxation of every edge - a node is relaxed several times before Closed catches
		// it, so a long strip paid for the same unchanged answer again and again. This is
		// #171's complaint about EdgeCost's re-sampling, in the one place that survived it.
		//
		// PER SEARCH, NOT CACHED ON THE EDGE: runway-ness depends on the PROFILE, which
		// changes when a road is re-profiled - an open invalidation trigger with no writer
		// positioned to catch it, unlike FGuidelineEdge::Length's closed set of three. A
		// stale true would refuse taxiways for the rest of the session.
		auto IsRunwayEdge = [&Network, &RunwaySeeds](FRoadSegmentId Seed)
		{
			if (!Seed.IsSet())
			{
				return false;
			}
			if (const bool* Known = RunwaySeeds.Find(Seed.Index))
			{
				return *Known;
			}
			++GRunwaySeedResolveCountForTest;
			const bool bRunway = Network.IsRunwaySegment(Seed);
			RunwaySeeds.Add(Seed.Index, bRunway);
			return bRunway;
		};
```

Replace the avoidance test's body so it goes through the memo:

```cpp
			// Runway-derived edges are the strip itself. See ERunwayAvoidance for who may
			// taxi along one and when.
			if (Query.AvoidRunways != ERunwayAvoidance::None
				&& IsRunwayEdge(Edge->DerivedFrom)
				&& (Query.AvoidRunways == ERunwayAvoidance::All || IsRunwayHeld(Edge->DerivedFrom)))
			{
				return;
			}
```

Define the two accessors beside `ResetNodeVisitCountForTest`'s definitions in the `RouteSearch` namespace:

```cpp
	int32 RunwaySeedResolveCountForTest() { return GRunwaySeedResolveCountForTest; }
	void ResetRunwaySeedResolveCountForTest() { GRunwaySeedResolveCountForTest = 0; }
```

- [ ] **Step 5: Thread the map through both callers**

In `RunSearch`, beside the existing `TMap<int32, bool> RunwayInUse;` declaration (around line 310):

```cpp
	TMap<int32, bool> RunwaySeeds;
```

and add `RunwaySeeds` to the `ExpandNode(...)` call immediately after `RunwayInUse`.

Do the same in `FindToGoals` (the declaration around line 555, the call around line 588).

- [ ] **Step 6: Build and run the whole search suite**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RouteSearch
```

Expected: every existing `Airside.Model.RouteSearch.*` test still passes, and `RunwaySeedMemo` passes. **This task claims no behaviour change — a single moved test is a failure of that claim, not an expected cost.**

- [ ] **Step 7: Prove the memo test measures something**

Temporarily change `IsRunwayEdge` to skip its `RunwaySeeds.Find` early-out (always resolve). Re-run. Expected: `RunwaySeedMemo` FAILS with the count at 5, not 1. Revert and re-run to green.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h \
        Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp \
        Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp
git commit -m "perf(routing): memoise runway-seed resolution per search

IsRunwaySegment was a slot lookup plus a profile resolve on every
relaxation of every edge whenever avoidance was on - #171's complaint in
the one place that survived it. No behaviour change; the counter test
measures the memo rather than naming it.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `FRouteQuery` carries an errand and its policy

Adds the fields and a new `For` overload that resolves the table. The **old** `For` overload stays, so nothing else has to change yet and the tree keeps building. No refusals yet — those arrive in Task 6, once every caller names an errand.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h` (`FRouteQuery`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp:357-366` (`FRouteQuery::For`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp` (one test)

**Interfaces:**
- Consumes: `ERouteErrand`, `FRoutePolicy`, `EOccupancyUse` from Task 1.
- Produces: `FRouteQuery::Errand` (`ERouteErrand`), `FRouteQuery::Policy` (`FRoutePolicy`), `FRouteQuery::RunwayPenalty` (`double`), and the overload `FRouteQuery::For(ERouteErrand, FGuidelineNodeId Start, FGuidelineNodeId Goal, const FAirframe&, ETraversalClass)`. Later tasks call **only** this overload.

- [ ] **Step 1: Write the failing test**

Append to `RoutePolicyTest.cpp`, before the final `#endif`, and add `#include "Model/RouteSearch.h"` and `#include "Model/Airframe.h"` to its includes:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoutePolicyQueryTest,
	"Airside.Model.RoutePolicy.QueryResolvesTheTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoutePolicyQueryTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe;
	Airframe.Wingspan = 3000.0;

	const FRouteQuery TaxiIn = FRouteQuery::For(
		ERouteErrand::ArrivalTaxiIn, FGuidelineNodeId(), FGuidelineNodeId(),
		Airframe, ETraversalClass::Aircraft);

	TestEqual(TEXT("the errand is carried, so the search can refuse an unset one"),
		TaxiIn.Errand, ERouteErrand::ArrivalTaxiIn);
	TestEqual(TEXT("avoidance comes from the table, not from the caller"),
		TaxiIn.AvoidRunways, ERunwayAvoidance::All);
	TestEqual(TEXT("the resolved policy travels with the query for the cost to read"),
		TaxiIn.Policy.Occupancy, EOccupancyUse::Never);
	TestEqual(TEXT("the factory still fills wingspan from the airframe"),
		TaxiIn.Wingspan, 3000.0);

	const FRouteQuery Backtrack = FRouteQuery::For(
		ERouteErrand::DepartureBacktrack, FGuidelineNodeId(), FGuidelineNodeId(),
		Airframe, ETraversalClass::Aircraft);
	TestEqual(TEXT("the one errand that must use a strip is not given a filter"),
		Backtrack.AvoidRunways, ERunwayAvoidance::None);

	// TWO DEFAULTS THAT MUST AGREE, checked rather than trusted. FRouteQuery carries its own
	// RunwayPenalty because a query built without any FTrafficRules to hand must still be
	// costed the same way one built with them is - and two constants typed in two files are
	// how the Piper's numbers ended up different at seven sites.
	TestEqual(TEXT("FRouteQuery's penalty default equals FTrafficRules'"),
		FRouteQuery().RunwayPenalty, FTrafficRules().RunwayPenalty);

	return true;
}
```

Also add `#include "Model/TrafficRules.h"` for that last assertion.

- [ ] **Step 2: Run the test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoutePolicy`

Expected: build failure — `'Errand': is not a member of 'FRouteQuery'`.

- [ ] **Step 3: Add the fields to `FRouteQuery`**

In `RouteSearch.h`, add `#include "Model/RoutePolicy.h"`… **no.** `RoutePolicy.h` includes `RouteSearch.h`, so including it back is a cycle. Instead add above `FRouteQuery`:

```cpp
enum class ERouteErrand : uint8;
struct FRoutePolicy;
```

and because `FRouteQuery` needs `FRoutePolicy` **by value**, move the policy include into the .cpp and declare the member through the generated header instead — UHT cannot see a forward-declared `USTRUCT` by value.

**The correct resolution, decided here so the implementer does not have to:** move `ERunwayAvoidance` out of `RouteSearch.h` into `RoutePolicy.h`, and have `RouteSearch.h` include `RoutePolicy.h`. `RoutePolicy.h` then includes only `CoreMinimal.h` and its own generated header. This is the direction the dependency wants to run — the policy vocabulary is the lower layer, and the search consumes it.

Apply that now:

1. In `RoutePolicy.h`, replace `#include "Model/RouteSearch.h"` with nothing, and **move** the whole `ERunwayAvoidance` enum and its doc comment from `RouteSearch.h` into `RoutePolicy.h`, above `ERouteErrand`. The comment travels with it unchanged — a refactor that drops a WHY comment has to say which and why.
2. In `RouteSearch.h`, add `#include "Model/RoutePolicy.h"` and delete the moved enum.
3. In `RoutePolicy.cpp`, the include stays `#include "Model/RoutePolicy.h"`.

Then add to `FRouteQuery`, immediately after the `Goal` member:

```cpp
	/**
	 * What this route is FOR. Everything policy-shaped below is derived from it.
	 *
	 * UNSET IS REFUSED by Find and FindToGoals - see their own guards. The four call sites
	 * that used to say nothing about runway avoidance, and silently got the permissive
	 * answer, are the reason the default cannot be a usable one.
	 */
	UPROPERTY() ERouteErrand Errand = ERouteErrand::Unset;

	/**
	 * The resolved row for Errand, filled by For(). Carried on the query rather than
	 * re-resolved inside the search so that one lookup answers the avoidance test, the cost
	 * term and the occupancy check - three readers, one answer, which is the same rule
	 * FRouteStep::EndVertex follows about the polyline.
	 */
	UPROPERTY() FRoutePolicy Policy;

	/**
	 * Multiplier on a runway edge's length, applied only when Policy.bPenaliseRunways.
	 *
	 * MIRRORS FTrafficRules::RunwayPenalty exactly as CongestionWeight below mirrors
	 * FTrafficRules::CongestionWeight: a query built with no rules to hand must still be
	 * costed the way one built with them is. Airside.Model.RoutePolicy.QueryResolvesTheTable
	 * asserts the two defaults agree, because two constants in two files is how the Piper's
	 * figures ended up different at seven sites.
	 *
	 * NEVER BELOW 1.0. A multiplier under one would make an edge cheaper than its own chord
	 * and break the straight-line heuristic's admissibility silently - the first pop would
	 * stop being optimal and nothing would say so.
	 */
	UPROPERTY() double RunwayPenalty = 10.0;
```

- [ ] **Step 4: Add the new `For` overload**

In `RouteSearch.h`, beside the existing `For` declaration:

```cpp
	/**
	 * Start/Goal/Class/Wingspan AND the whole routing policy, from the errand.
	 *
	 * THE ONLY FACTORY PRODUCTION CODE MAY USE. The errand-less overload above survives for
	 * the tests that are about graph shape and nothing else, and Task 6 replaces those with
	 * ERouteErrand::GraphProbe before deleting it.
	 */
	static FRouteQuery For(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
		const FAirframe& Airframe, ETraversalClass Class);
```

In `RouteSearch.cpp`, beside the existing definition:

```cpp
FRouteQuery FRouteQuery::For(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
	const FAirframe& Airframe, ETraversalClass Class)
{
	FRouteQuery Query = For(Start, Goal, Airframe, Class);
	Query.Errand = Errand;
	Query.Policy = FRoutePolicy::For(Errand);

	// THE TABLE OVERWRITES THE FIELD, rather than the field being set beside it. AvoidRunways
	// stays a public member because the search reads it in the hot loop and because the
	// errand-less overload still exists; making the table the only writer of it here is what
	// stops a caller setting both and getting whichever happened to be assigned last.
	Query.AvoidRunways = Query.Policy.Avoidance;
	return Query;
}
```

Add `#include "Model/RoutePolicy.h"` to `RouteSearch.cpp` if the header move in Step 3 did not already bring it in transitively — it did, via `RouteSearch.h`, so no change is expected here. Verify rather than assume.

- [ ] **Step 5: Add `RunwayPenalty` to `FTrafficRules`**

In `Plugins/Airside/Source/Airside/Public/Model/TrafficRules.h`, immediately after the `CongestionWeight` line:

```cpp
	/**
	 * What a runway edge costs, as a multiple of its length, on an errand whose policy
	 * allows one at all (FRoutePolicy::bPenaliseRunways).
	 *
	 * TEN IS AN ARGUMENT, NOT A MEASUREMENT: a taxiway detour is rarely ten times the strip
	 * it parallels, so ten sends a route round whenever round exists, and still lets the
	 * strip win when it is the only way through. It has not been judged against a real
	 * airport - see the spec's section 10.
	 *
	 * ClampMin 1.0: below one, a runway edge would cost less than its own chord and the
	 * search's straight-line heuristic would stop being admissible, silently.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0")) double RunwayPenalty = 10.0;
```

- [ ] **Step 6: Build twice and run**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Expected: `3 test(s) run, 0 failed` for the `RoutePolicy` filter, and the **whole** suite still green — this task wires nothing, so nothing may move. Read the `N test(s) run` line; do not trust the exit code.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoutePolicy.h \
        Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h \
        Plugins/Airside/Source/Airside/Public/Model/TrafficRules.h \
        Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp \
        Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp
git commit -m "feat(routing): FRouteQuery carries an errand and its resolved policy

New For(Errand, ...) overload resolves the table onto the query; the old
overload survives for Task 6 to replace. ERunwayAvoidance moves to
RoutePolicy.h with its comment, so the policy header is the lower layer.
No caller migrated yet, no behaviour change.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: The runway penalty in `EdgeCost`

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp:30-47` (`EdgeCost`), `:150` (its call site in `ExpandNode`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp` (two tests)

**Interfaces:**
- Consumes: `FRouteQuery::Policy`, `FRouteQuery::RunwayPenalty` (Task 3); `IsRunwayEdge` inside `ExpandNode` (Task 2).
- Produces: `EdgeCost` gains a trailing `bool bRunwayEdge` parameter.

- [ ] **Step 1: Write the failing tests**

Append to `RouteStepDistanceTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwayPenaltyTest,
	"Airside.Model.RouteSearch.RunwayPenalty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwayPenaltyTest::RunTest(const FString& Parameters)
{
	// A(0,0) to B(20000,0). Along the strip is SHORT; the detour via D is longer but legal.
	// The penalty is the only thing that can make the longer way win, which is what makes
	// this measure the rule rather than name it: with RunwayPenalty at 1.0 it must go red.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	// D at 8000 north: the detour is roughly 34000 uu against the strip's ~22000, so it
	// loses on plain length and wins at a penalty of ten. A margin, not a hair: a detour
	// that only just won would be measuring floating-point noise.
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(10000.0, 8000.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, D); TestGraph::Join(*Net, D, B);
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	auto ViaRunway = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 3 && Plan.Steps[0].To == R1; };
	auto ViaDetour = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 2 && Plan.Steps[0].To == D; };

	FAirframe Airframe;

	// GraphProbe: no filter, no penalty. The strip is ordinary line and the short way wins.
	FRouteQuery Probe = FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("with no policy at all the strip is ordinary line - the short way"),
		ViaRunway(RouteSearch::Find(*Net, Probe)));

	// PlayerIssued: no filter, but a penalty. Same graph, opposite answer.
	FRouteQuery Player = FRouteQuery::For(ERouteErrand::PlayerIssued, A, B, Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("the penalty alone sends a player-issued route round the strip"),
		ViaDetour(RouteSearch::Find(*Net, Player)));

	// AND THE PENALTY IS THE THING DOING IT, not the errand: turn the multiplier off and the
	// same errand takes the strip again. Without this the test would still pass on a build
	// where PlayerIssued had quietly been given an All filter instead.
	Player.RunwayPenalty = 1.0;
	TestTrue(TEXT("at a multiplier of one the same errand takes the strip"),
		ViaRunway(RouteSearch::Find(*Net, Player)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwayOnlyWayTest,
	"Airside.Model.RouteSearch.RunwayPenaltyStillRoutesWhenItIsTheOnlyWay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwayOnlyWayTest::RunTest(const FString& Parameters)
{
	// THE DEGRADATION PlayerIssued EXISTS FOR. No detour at all: a filter would report
	// Unreachable and read as a broken tool, where a penalty takes the only way there is.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	FAirframe Airframe;

	const FRoutePlan Player = RouteSearch::Find(*Net,
		FRouteQuery::For(ERouteErrand::PlayerIssued, A, B, Airframe, ETraversalClass::Aircraft));
	TestTrue(TEXT("a penalty is expensive, not impossible: the only way through is still found"),
		Player.IsValid());

	// THE CONTRAST THAT GIVES THAT ITS MEANING. An errand with a filter reports Unreachable
	// on the very same graph - so the assertion above is about the penalty, not about the
	// graph happening to be routable.
	const FRoutePlan TaxiIn = RouteSearch::Find(*Net,
		FRouteQuery::For(ERouteErrand::ArrivalTaxiIn, A, B, Airframe, ETraversalClass::Aircraft));
	TestEqual(TEXT("a filtered errand refuses the same graph outright"),
		TaxiIn.Result, ERouteResult::Unreachable);

	return true;
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RouteSearch.RunwayPenalty`

Expected: **2 tests run, 2 failed.** `RunwayPenalty` fails on "the penalty alone sends a player-issued route round the strip" (no penalty exists yet, so the strip wins); `RunwayPenaltyStillRoutesWhenItIsTheOnlyWay` passes its first assertion and may pass entirely — if it does, note it and continue; it is a guard against a later regression, not a driver of this step.

- [ ] **Step 3: Apply the penalty in `EdgeCost`**

Replace `EdgeCost`'s signature and body in `RouteSearch.cpp`:

```cpp
	double EdgeCost(const FGuidelineEdge& Edge, FGuidelineEdgeId EdgeId, const FRouteQuery& Query,
		bool bRunwayEdge)
	{
		double Length = Edge.Length;

		// A RUNWAY THIS ERRAND IS STILL ALLOWED TO USE COSTS A MULTIPLE OF ITS LENGTH.
		// Layered UNDER the avoidance filter rather than replacing it: a filter is absolute
		// and is what an arrival's taxi needs, but an errand that slipped through without
		// one should degrade to a detour rather than to a free taxi down the strip. The two
		// never both apply - a policy carrying All and a penalty is refused by the table
		// test, because the edge is gone before this line is reached.
		//
		// MULTIPLICATIVE, NOT ADDITIVE, so a long strip costs proportionally more than a
		// short one; a flat charge would be swallowed by a 3km runway. Still >= Length, so
		// the straight-line heuristic stays admissible and the first pop stays optimal -
		// FTrafficRules::RunwayPenalty is ClampMin 1.0 for exactly that reason.
		if (bRunwayEdge && Query.Policy.bPenaliseRunways)
		{
			Length *= FMath::Max(1.0, Query.RunwayPenalty);
		}

		// Congestion: what others hold on this edge, weighted. Additive and non-negative,
		// so the straight-line heuristic stays admissible and the first pop stays optimal.
		// Nodes are not costed - a held node is a moment, a held edge is a queue.
		//
		// The querying agent's OWN claims are excluded by HeldLengthOn, or an agent
		// replanning out of a jam would be charged for the very line it is standing on and
		// route round itself. With a null table this is bitwise the search that ran before
		// occupancy existed - see Airside.Model.RouteSearch.OccupancyCost's last assertion.
		if (Query.Occupancy != nullptr)
		{
			Length += Query.CongestionWeight * Query.Occupancy->HeldLengthOn(EdgeId, Query.QueryingAgent);
		}

		return Length;
	}
```

> Note `FMath::Max(1.0, ...)` as well as the `ClampMin`: the clamp guards the Details panel, and a query built in C++ never passes through it.

- [ ] **Step 4: Pass the memoised answer at the call site**

In `ExpandNode`, the avoidance test already computes runway-ness through `IsRunwayEdge`. Hoist it so both readers share one call, replacing the avoidance block and the `EdgeCost` line:

```cpp
			// ONE ANSWER, TWO READERS: the filter below and the cost term. Asking the memo
			// twice would be cheap, but reading it once is what guarantees the edge the
			// filter judged is the edge the cost charged for.
			const bool bRunwayEdge = IsRunwayEdge(Edge->DerivedFrom);

			// Runway-derived edges are the strip itself. See ERunwayAvoidance for who may
			// taxi along one and when.
			if (Query.AvoidRunways != ERunwayAvoidance::None
				&& bRunwayEdge
				&& (Query.AvoidRunways == ERunwayAvoidance::All || IsRunwayHeld(Edge->DerivedFrom)))
			{
				return;
			}

			if (!bIgnoreWingspan && ExceedsWingspan(*Edge, Query.Wingspan))
			{
				return;
			}

			const double Cost = EdgeCost(*Edge, EdgeId, Query, bRunwayEdge);
```

- [ ] **Step 5: Build and run the search suite**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RouteSearch
```

Expected: all green, including the two new tests and the pre-existing `RunwayAvoidance`, `OccupancyCost` and `RunwaySeedMemo`.

> **If `RunwaySeedMemo` now reports 0 resolves:** Step 4 hoisted `IsRunwayEdge` above the `AvoidRunways != None` short-circuit, so it is now called for every edge rather than only when avoidance is on. That is intended — the count is per *seed*, not per call, so it stays 1. A 0 means the hoist landed below an early `return`; re-check the ordering.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp \
        Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp
git commit -m "feat(routing): a runway edge costs a multiple of its length

Layered under the avoidance filter, not replacing it: an errand without a
filter now degrades to a detour instead of a free taxi down the strip.
Multiplicative and >= 1.0, so the straight-line heuristic stays admissible.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Migrate all ten production call sites

The behaviour change lands here. Every production query names an errand; five sites gain a restriction or a cost term they never declared.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/ArrivalPlanner.cpp:32-36`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/DeparturePlanner.cpp:60-64, 88, 106`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/PushbackPlanner.cpp:173, 202`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTrafficRebuild.cpp:81-98, 609-614`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp:222-240, 328-333`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp:532-569`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h:211`, `Public/Present/RoadNetworkActor.h:294`, `Public/Tool/RoadEditTarget.h:368`, `Public/Testing/AirsideTestWorld.h:190`, `Private/Present/RoadNetworkActor.cpp:941-944`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/TrafficForwardersTest.cpp:167-168`

**Interfaces:**
- Consumes: `FRouteQuery::For(ERouteErrand, ...)` from Task 3.
- Produces: `FindRoute` gains a trailing `ERouteErrand Errand = ERouteErrand::PlayerIssued` parameter on all four declarations and the two definitions.

- [ ] **Step 1: Migrate `ArrivalPlanner.cpp`**

Replace the hand-built query at `:32-36`. Keep the existing comment about why `For` was not used — it is still true of the errand-less overload and now needs one added sentence:

```cpp
		// Built by hand rather than FRouteQuery::For: that factory also takes a Goal, and
		// there isn't ONE here - FindToGoals takes the whole Candidates set instead of a
		// single Query.Goal. See RouteSearch::FindToGoals (#190, deferred from #171/#201):
		// ONE multi-goal search from From replaces the old one-Find()-per-stand loop, which
		// stayed O(exits x stands) searches per dispatch, per re-offer, per plan re-resolve
		// even after #201 made each individual search cheap.
		//
		// THE POLICY STILL COMES FROM THE TABLE, hand-built or not: the errand is set and
		// FRoutePolicy::For resolves it, so this site cannot drift from the one list even
		// though it cannot use the factory.
		FRouteQuery Query;
		Query.Start = From;
		Query.Class = ETraversalClass::Aircraft;
		Query.Wingspan = Airframe.Wingspan;
		Query.Errand = ERouteErrand::ArrivalTaxiIn;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.AvoidRunways = Query.Policy.Avoidance;
```

Add `#include "Model/RoutePolicy.h"` to the file's includes.

- [ ] **Step 2: Migrate `DeparturePlanner.cpp`**

Change `TryRoute` to take an errand instead of an avoidance:

```cpp
		auto TryRoute = [&](FGuidelineNodeId Candidate, ERouteErrand Errand)
		{
			return RouteSearch::Find(Network, FRouteQuery::For(Errand, Start, Candidate, Airframe, Class));
		};
```

At `:88`: `TryRoute(Candidate, ERouteErrand::DepartureToEntry)`.
At `:106`: `TryRoute(Candidate, ERouteErrand::DepartureBacktrack)`.

Add `#include "Model/RoutePolicy.h"`.

- [ ] **Step 3: Migrate `PushbackPlanner.cpp`** — both sites gain `All`, the reported bug

At `:173`:

```cpp
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::PushbackClear, PoseNode, Goal, Airframe, Class);
		Query.BannedEdge = Taken;
```

At `:202`:

```cpp
	// THE ERRAND, AND IT IS NOT PushbackClear: this is the taxi out to the runway entry, and
	// until 2026-09-21 it declared no runway policy at all and got the permissive one - the
	// long taxi that let an aeroplane drive down the strip, which is the bug this table was
	// built for.
	FRouteQuery Onward = FRouteQuery::For(ERouteErrand::PushbackTaxiOut, PushEnd, Goal, Airframe, Class);
	Out.TaxiOutRoute = RouteSearch::Find(Network, Onward);
```

Add `#include "Model/RoutePolicy.h"`.

- [ ] **Step 4: Migrate `GroundTrafficRebuild.cpp`**

At `:81`, add the errand and delete the now-redundant hand-set avoidance, keeping the whole WHY comment that explains the `Held` choice — it is the justification for the table's row and must not be lost:

```cpp
	FRouteQuery Query = FRouteQuery::For(ERouteErrand::Replan,
		UGroundTraffic::StepFromNode(Plan, SpliceStep), Agent.GoalNode, Agent.Airframe, Agent.Class);
	Query.BannedEdge = BannedEdge;
	Query.BannedNode = BannedNode;

	// NEVER ALONG A RUNWAY SOMEBODY ELSE HOLDS. The first replan this resolver ever made in
	// play looped an arrival round a runway's end taxiway and back over a runway-derived
	// edge; that edge re-reserved the strip (spec §3.1, route one) against the departure
	// waiting at the bar, which was the very agent the loop was meant to get round. That
	// departure HELD the strip - a bar claim is a reservation on the chain - which is what
	// Held reads. The outright ban this replaced (All) also refused a free runway end as a
	// turnaround, and sent an aircraft round the whole taxiway loop past one it could have
	// used (samples/routing.png, 2026-09-07). Crossings are turn paths and nodes, not
	// runway edges, so they stay open either way. See ERunwayAvoidance.
	//
	// THE CHOICE NOW LIVES IN FRoutePolicy::For(Replan) and this paragraph is its
	// justification; the assignment that used to stand here would be a second source of
	// truth for it.

	// THE COST TERM IS THE POINT OF REPLANNING, not the ban. The ban removes the one edge
	// the caller knows is hopeless; the congestion cost is what stops the new route from
	// being the next queue along, which a plain shortest path would walk straight into.
	Query.WithCongestion(Occupancy, Agent.Id, Rules.CongestionWeight);
	Query.RunwayPenalty = Rules.RunwayPenalty;
```

At `:609`, in `ReResolvePlan`:

```cpp
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::RebuildReResolve,
			UGroundTraffic::StepFromNode(Plan, Failed), Agent.GoalNode, Agent.Airframe, Agent.Class);

		// The congestion term, as ReplanAt takes it: the guidelines that survived the rebuild
		// by handle - every hand-drawn one - still carry real queues, and a re-routed arrival
		// should be steered round them rather than into the back of one.
		Query.WithCongestion(Occupancy, Agent.Id, Rules.CongestionWeight);
		Query.RunwayPenalty = Rules.RunwayPenalty;
```

Add `#include "Model/RoutePolicy.h"`.

- [ ] **Step 5: Migrate `FuelService.cpp`**

At `:222`, the depot comparison — keep the `NO OCCUPANCY WEIGHT` comment verbatim, it is now the justification for the `CandidateComparison` row:

```cpp
		FRouteQuery Query;
		Query.Start = Instance.PoseNode;
		Query.Goal = StandFuel;
		Query.Class = ETraversalClass::GroundVehicle;
		Query.Errand = ERouteErrand::CandidateComparison;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.AvoidRunways = Query.Policy.Avoidance;

		// 0 IS UNLIMITED, and a road guideline carries no span limit either, so neither side
		// of that comparison means anything for a van.
		Query.Wingspan = 0.0;

		// NEVER ALONG A STRIP. A truck crossing a runway at a junction is unaffected - a
		// crossing is a turn path and a node, and turn paths carry no DerivedFrom - but
		// taxiing DOWN one is not something a fuel job may plan.

		// NO OCCUPANCY WEIGHT, deliberately. Which depot is nearest is a fact about the
		// airport's SHAPE, not about who happens to be on the road this instant; a
		// congestion-weighted length would make the chosen depot flicker between ticks and
		// the log unreadable. Congestion is the arbiter's job once the truck is under way.
		// EOccupancyUse::Never on this errand's row is that rule, and the search REFUSES a
		// table passed alongside it rather than ignoring one.
```

At `:328`, the route home — this **gains** the congestion term, so it must now supply the table:

```cpp
		FRouteQuery Query;
		Query.Start = Truck->GoalNode;
		Query.Goal = Home->PoseNode;
		Query.Class = ETraversalClass::GroundVehicle;
		Query.Errand = ERouteErrand::VehicleToJob;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.AvoidRunways = Query.Policy.Avoidance;

		// A DRIVE, NOT A COMPARISON, so unlike the depot choice above this one takes the
		// table: the truck is committed to going home and should go round the queue that is
		// there rather than into the back of it. The flicker argument covers a WINNER being
		// re-picked every tick, which a committed route is not.
		Query.WithCongestion(Traffic.GetOccupancy(), TruckId, Traffic.Rules.CongestionWeight);
		Query.RunwayPenalty = Traffic.Rules.RunwayPenalty;
		Plan = RouteSearch::Find(Network, Query);
```

Add `#include "Model/RoutePolicy.h"`.

> If `Traffic` is not in scope at `:328` under that name, use whatever `UGroundTraffic&` the enclosing function holds — `RedirectAgent` is called on it twelve lines below, so one exists.

- [ ] **Step 6: Migrate `FindRoute` and its four declarations**

`FindRoute` has **no production caller** — only four tests — and its aircraft/vehicle branch is pinned by `TrafficForwardersTest.cpp:167-168`. The branch is replaced by the errand, not deleted.

In `RoadEditFacadeSurfaces.cpp:532`:

```cpp
FRoutePlan URoadEditFacade::FindRoute(
	FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class, double Wingspan,
	ERouteErrand Errand) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr)
	{
		return FRoutePlan();
	}

	FRouteQuery Query;
	Query.Start = Start;
	Query.Goal = Goal;
	Query.Class = Class;
	Query.Wingspan = Wingspan;
	Query.Errand = Errand;
	Query.Policy = FRoutePolicy::For(Errand);
	Query.AvoidRunways = Query.Policy.Avoidance;

	// THE ERRAND DECIDES, NOT THE CLASS. This used to read "vehicles always route with the
	// table; aircraft never" and branch on ETraversalClass - the ground-traffic spec's §4
	// rule, implemented locally here and at three other sites. It was never quite true: a
	// fuel truck choosing a depot must NOT route with the table, or the winner flickers
	// between ticks. The axis that survives every site is whether the route may still
	// change, and that is what the errand names.
	//
	// TrafficModelProvider, not Actor().GetTraffic()->GetModel() directly: this class must
	// not reach past Network/History for anything else (#104, and see the class comment) -
	// ARoadNetworkActor wires the provider once, right after it creates Traffic, the same
	// way it wires OnChanged right after creating Facade.
	//
	// QueryingAgent stays 0: nothing has been dispatched yet, so there is no agent whose own
	// claims should be discounted from the cost.
	if (Query.Policy.Occupancy == EOccupancyUse::Required && TrafficModelProvider)
	{
		if (const UGroundTraffic* Model = TrafficModelProvider())
		{
			Query.Occupancy = &Model->GetOccupancy();
			Query.CongestionWeight = Model->Rules.CongestionWeight;
			Query.RunwayPenalty = Model->Rules.RunwayPenalty;
		}
	}

	return RouteSearch::Find(*Network, Query);
}
```

Add the parameter, with its default, to all four declarations and the actor's forwarder:

- `Public/Present/RoadEditFacade.h:211`
- `Public/Present/RoadNetworkActor.h:294`
- `Public/Tool/RoadEditTarget.h:368`
- `Public/Testing/AirsideTestWorld.h:190` (a stub returning `FRoutePlan()`; just widen the signature)

each becoming, with the same doc comment they already carry plus one line:

```cpp
	/** ... existing comment ... Errand names the routing policy; see FRoutePolicy. */
	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan,
		ERouteErrand Errand = ERouteErrand::PlayerIssued) const;
```

And `RoadNetworkActor.cpp:941`:

```cpp
	return Facade->FindRoute(Start, Goal, Class, Wingspan, Errand);
```

- [ ] **Step 7: Keep `TrafficForwardersTest` asserting what it always did**

At `TrafficForwardersTest.cpp:167-168`, the van and the aeroplane must still be costed differently — now by errand rather than by class:

```cpp
	// THE ERRAND, NOT THE CLASS, since 2026-09-21. This pair has always asserted that a van
	// sent to a job is costed by the occupancy table and an aeroplane's route is not; the
	// rule now lives in FRoutePolicy rather than in an ETraversalClass branch inside
	// FindRoute, and the assertion below is unchanged.
	const FRoutePlan VanRoute = Actor->FindRoute(RA, RC, ETraversalClass::GroundVehicle, 0.0, ERouteErrand::VehicleToJob);
	const FRoutePlan PlaneRoute = Actor->FindRoute(RA, RC, ETraversalClass::Aircraft, 0.0, ERouteErrand::PlayerIssued);
```

- [ ] **Step 8: Build and run the whole suite**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Expected: **some tests will move**, and that is this task's whole point. For each failure, classify it before touching it:

- A test asserting a route **across a strip** through `PushbackTaxiOut`, `RebuildReResolve` or `PlayerIssued` — the behaviour change the spec predicted. Update the expectation and say in the commit which errand changed it.
- Anything else — a regression. Do not relax the test; find the cause.

Record the before/after of the summary line (`N test(s) run, N failed, N crashed`) for the commit message.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(routing): every production query names its errand

Ten call sites now resolve policy from FRoutePolicy instead of setting it
by hand. Five change behaviour: PushbackClear and PushbackTaxiOut gain an
All filter (the reported bug - an aeroplane taxiing down the strip),
RebuildReResolve gains Held, FuelService's route home gains the congestion
term, and PlayerIssued gains the runway penalty. FindRoute's
ETraversalClass branch is replaced by the errand, not deleted.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Migrate the tests, delete the old factory, turn on the refusals

Once no caller can omit an errand, `Unset` becomes unreachable and the search can refuse it.

**Files:**
- Modify: every test file with a hand-built `FRouteQuery` — `RouteSearchTest.cpp` (11), `ServiceLinkTest.cpp` (6), `RouteStepDistanceTest.cpp` (6), `AnchorLinkTest.cpp` (3), `InspectorWidgetTest.cpp` (2), `SimTimeScaleTest.cpp` (2), `LeadInSweepTest.cpp` (2), `FuelDepotAnchorTest.cpp` (2), `DepartAgentTest.cpp` (2), `AgentRedirectTest.cpp` (2), `VehicleAgentTest.cpp` (1), `TruckCrossingTest.cpp` (1)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteSearch.h` (delete the errand-less `For`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteSearch.cpp` (`RunSearch`, `FindToGoals`, delete the old `For`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RoutePolicyTest.cpp` (one test)

**Interfaces:**
- Consumes: everything from Tasks 1, 3, 5.
- Produces: no new API. The errand-less `FRouteQuery::For` ceases to exist.

- [ ] **Step 1: Write the failing test**

Append to `RoutePolicyTest.cpp`, before the final `#endif`. It needs an unbuffered log spy — a buffered one delivers lines after the spy is gone, which is the #216 flake:

```cpp
namespace
{
	/**
	 * Catches Error lines while it is alive.
	 *
	 * CanBeUsedOnMultipleThreads OVERRIDDEN TRUE, and that is not optional: without it UE's
	 * dedicated log thread buffers and delivers lines AFTER this object has been destroyed,
	 * which is the #216 flake - the test passes or fails on timing rather than on behaviour.
	 */
	class FErrorSpy : public FOutputDevice
	{
	public:
		FErrorSpy() { GLog->AddOutputDevice(this); }
		virtual ~FErrorSpy() { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Error) { Errors.Add(FString(V)); }
		}
		TArray<FString> Errors;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteErrandRefusalTest,
	"Airside.Model.RoutePolicy.SearchRefusesABadQuery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteErrandRefusalTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 1000.0, 0.0);
	TestGraph::Join(*Net, A, B);

	FAirframe Airframe;

	// The control: a properly-named errand over this graph DOES route, so every refusal
	// below is about the query and not about the graph being empty.
	{
		const FRoutePlan Good = RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Airframe, ETraversalClass::Aircraft));
		if (!TestTrue(TEXT("the control routes, so the refusals below are about the query"), Good.IsValid()))
		{
			return false;
		}
	}

	// 1. No errand at all - the four call sites that used to say nothing.
	{
		FErrorSpy Spy;
		FRouteQuery Q;
		Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
		const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
		TestFalse(TEXT("a query with no errand does not route"), Plan.IsValid());
		TestTrue(TEXT("and says so at Error, because a Warning would not fail a test run"),
			Spy.Errors.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("no errand")); }));
	}

	// 2. Required occupancy, no table.
	{
		FErrorSpy Spy;
		FRouteQuery Q = FRouteQuery::For(ERouteErrand::VehicleToJob, A, B, Airframe, ETraversalClass::GroundVehicle);
		const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
		TestFalse(TEXT("an errand that must weigh congestion will not route without the table"), Plan.IsValid());
		TestTrue(TEXT("and says so at Error"), Spy.Errors.Num() > 0);
	}

	// 3. Never occupancy, table supplied anyway. THE HALF THAT WOULD ROT if only the first
	// were written: a caller that supplied a table believes it is being weighted by it.
	{
		FErrorSpy Spy;
		FTrafficOccupancy Table;
		FRouteQuery Q = FRouteQuery::For(ERouteErrand::CandidateComparison, A, B, Airframe, ETraversalClass::GroundVehicle);
		Q.WithCongestion(Table, 1, 2.0);
		const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
		TestFalse(TEXT("an errand costed on shape alone refuses a table rather than ignoring it"), Plan.IsValid());
		TestTrue(TEXT("and says so at Error"), Spy.Errors.Num() > 0);
	}

	// 4. FindToGoals guards identically - it is a second entry point, and a guard on one
	// entry point is exactly the "check where a list is CONSUMED" bug wearing a new hat.
	{
		FErrorSpy Spy;
		FRouteQuery Q;
		Q.Start = A; Q.Class = ETraversalClass::Aircraft;
		TArray<FGoalReach> Reach;
		const FMultiGoalSearch Search = RouteSearch::FindToGoals(*Net, Q, { B }, Reach);
		TestEqual(TEXT("FindToGoals settles nothing on a query with no errand"), Search.Arrived.Num(), 0);
		TestTrue(TEXT("and says so at Error"), Spy.Errors.Num() > 0);
	}

	return true;
}
```

Add includes for `Model/TrafficOccupancy.h` and the test fixtures header.

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoutePolicy.SearchRefusesABadQuery`

Expected: FAIL — "a query with no errand does not route" fails, because the search currently routes it happily.

- [ ] **Step 3: Migrate every test query**

For each of the twelve files listed above, give each hand-built `FRouteQuery` an errand:

- A test about **graph shape** — reachability, distances, splices, wingspan, one-way direction — gets `Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand);`. That is `RouteSearchTest.cpp` throughout, and most of the others.
- A test about **policy** names the errand it means. In `RouteStepDistanceTest.cpp`, the existing `RunwayAvoidance` test sets `AvoidRunways` directly through three values and pairs `Held` with an occupancy table; leave those direct assignments in place — it is testing `ERunwayAvoidance` itself, below the policy layer — but give the query `Q.Errand = ERouteErrand::Replan;` so the `Held` + table combination satisfies the occupancy check, and `Q.Policy.Avoidance` is then overwritten per-case by the assignments that follow.

> **Watch for the occupancy check.** Any test that sets `Q.Occupancy` must use an errand whose row is `Required` (`Replan`, `RebuildReResolve`, `VehicleToJob`), or the search now refuses it. `OccupancyCost` in `RouteSearchTest.cpp` is the main one.

- [ ] **Step 4: Add the guards**

In `RouteSearch.cpp`, add a shared helper in the anonymous namespace:

```cpp
	/**
	 * Is this query answerable at all? Logged and refused rather than best-guessed.
	 *
	 * ONE FUNCTION, TWO ENTRY POINTS. Find and FindToGoals are both full searches and both
	 * must refuse the same things; a guard written into one of them is precisely the
	 * "check where a list is CONSUMED" failure CLAUDE.md lists three previous outings of.
	 *
	 * ERROR, NOT WARNING: FAutomationTestBase's warning-fails-the-test flag is false here and
	 * nothing sets it, so a Warning would let a silently permissive route ship green.
	 */
	bool IsQueryAnswerable(const FRouteQuery& Query)
	{
		if (Query.Errand == ERouteErrand::Unset)
		{
			UE_LOG(LogAirside, Error,
				TEXT("Route query has no errand (node %d -> %d); refusing. See FRoutePolicy."),
				Query.Start.Index, Query.Goal.Index);
			return false;
		}

		const bool bWants = Query.Policy.Occupancy == EOccupancyUse::Required;
		const bool bHas = Query.Occupancy != nullptr;
		if (bWants != bHas)
		{
			// BOTH WAYS. Required-without-a-table is the obvious half; Never-with-one is the
			// more useful, because a caller that went to the trouble of supplying occupancy
			// believes it is being weighted by it, and silently dropping the pointer leaves
			// that caller reasoning about a cost term the search never applied.
			UE_LOG(LogAirside, Error,
				TEXT("Route errand %d %s the occupancy table but %s given one; refusing."),
				static_cast<int32>(Query.Errand),
				bWants ? TEXT("requires") : TEXT("must not read"),
				bHas ? TEXT("was") : TEXT("was not"));
			return false;
		}

		return true;
	}
```

Call it first in `RunSearch` (returning a default `FRoutePlan()`, whose `Result` is already `NoStart`) and first in `FindToGoals` (returning a default `FMultiGoalSearch{}` after sizing `OutReach` to `Goals.Num()` with default `FGoalReach` entries, so a caller indexing `OutReach[i]` against `Goals[i]` does not read out of bounds).

Add `#include "AirsideLog.h"` to `RouteSearch.cpp` if not already present.

- [ ] **Step 5: Delete the errand-less factory**

Remove the `FRouteQuery::For(Start, Goal, Airframe, Class)` declaration from `RouteSearch.h` and its definition from `RouteSearch.cpp`. Fold its body into the errand-taking overload. The build now fails on any site that has not been migrated — which is the point.

- [ ] **Step 6: Build and run everything**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Expected: the whole suite green, at the same test count as before this task plus the new ones. A **dropped** count means a name collision ate a test.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(routing): refuse a query with no errand, delete the old factory

Both entry points guard, through one helper: Unset is refused, and the
occupancy pointer is checked BOTH ways - a Never errand handed a table is
refused rather than silently ignoring it. Test queries name GraphProbe or
the errand they mean.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: The `GraphProbe` lint

**Files:**
- Modify: `Tools/Check-Architecture.ps1` (new rule 11, before the `--- Verdict ---` block at line 408)

**Interfaces:**
- Consumes: `ERouteErrand::GraphProbe` from Task 1.
- Produces: nothing code-facing.

- [ ] **Step 1: Write the rule**

Insert before `# --- Verdict ---`:

```powershell
# --- 11. GraphProbe is for tests and tools only --------------------------------------------
# The permissive routing policy - no runway filter, no penalty, no congestion - survives only
# where it is NAMED. Four production call sites silently had exactly this policy before
# 2026-09-21 because FRouteQuery's defaults were the permissive ones, and an aeroplane taxied
# down a runway. GraphProbe is the one place that behaviour is still reachable, and a
# production caller reaching for it is that bug coming back wearing a name.
#
# Scoped to the two production modules the way rule 1 is: the test modules are its intended
# home, and Tool/ is exempt because a tool asking "is there any way from here to there" is a
# shape question with no agent behind it.
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        $dir = Join-Path $module $half
        foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
            if ($file.FullName -match '\\Tool\\') { continue }
            $hits = Select-String -Path $file.FullName -Pattern 'GraphProbe'
            foreach ($h in $hits) {
                # A WHY comment naming it - such as the enumerator's own doc comment in
                # RoutePolicy.h - is not a use of it. Same exemption as rules 5 and 7.
                if ($h.Line.Trim().StartsWith('//') -or $h.Line.Trim().StartsWith('*')) { continue }
                $failures.Add("graph-probe: $($file.FullName):$($h.LineNumber) ERouteErrand::GraphProbe is for tests and Tool/ only - production code names the errand it means: $($h.Line.Trim())")
            }
        }
    }
}
```

- [ ] **Step 2: Run the lint and verify it passes**

Run: `./Tools/Check-Architecture.ps1`

Expected: `Check-Architecture: 0 failure(s).` — the only non-comment `GraphProbe` occurrences are in test modules, and the enumerator's declaration in `RoutePolicy.h` is a bare `GraphProbe,` line. **If that declaration line trips the rule**, tighten the pattern to `ERouteErrand::GraphProbe` so only qualified *uses* match, and re-run.

- [ ] **Step 3: Prove the rule measures something**

Temporarily add `// ERouteErrand::GraphProbe` **without** the leading `//` — i.e. a bare line `const ERouteErrand Probe = ERouteErrand::GraphProbe;` — inside `Plugins/Airside/Source/Airside/Private/Model/ArrivalPlanner.cpp`. Re-run `./Tools/Check-Architecture.ps1`.

Expected: `1 failure(s)`, naming that file and line. Remove the line and re-run to zero.

- [ ] **Step 4: Commit**

```bash
git add Tools/Check-Architecture.ps1
git commit -m "chore(lint): GraphProbe is for tests and Tool/ only

The permissive routing policy survives only where it is named. Rule 11.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Verify against a real airport, then open the PR

The suite cannot judge `RunwayPenalty = 10.0`, and a graph change that passed 348 tests once still put kilometre-wide arcs across an apron. This task looks at the map.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-21-route-policy-design.md` (record the measured verdict on the constant)

- [ ] **Step 1: The authoritative pre-commit run**

```powershell
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line. The exit code is not evidence — a crashing test used to vanish and report green.

Record the number. It should be the pre-change count plus 7 (2 from Task 1, 1 from Task 2, 1 from Task 3, 2 from Task 4, 1 from Task 6).

- [ ] **Step 2: Reproduce the original report**

Open the editor, load the starter map, and run a full departure cycle — a pushback followed by a taxi out to the runway entry, which is `PushbackTaxiOut`, the errand that was permissive.

Then:

```
python Tools/Mcp.py log LogAirside
python Tools/Mcp.py shot after-fix.png
```

Expected: the taxi route does not run along the strip. **The screenshot is the evidence**; the test suite is not, because no test in it draws the starter map's actual geometry.

- [ ] **Step 3: Judge the penalty**

On the same map, issue a player route (`PlayerIssued`) between two points whose shortest path crosses a runway lengthways, with a parallel taxiway available.

- If it takes the taxiway: 10.0 is doing its job on this airport.
- If it still takes the strip: the detour is more than ten times longer, which on a real layout means the penalty is too low. Raise `FTrafficRules::RunwayPenalty` on the placed `ARoadNetworkActor` instance — not in the constructor — and re-test. **A UPROPERTY set in the level overrides the constructor default**, so read the instance value, not the header, when reporting what worked.

Record the value that worked and why in the spec's section 10, replacing "It ships at 10.0 on the grounds that..." with the measured verdict.

- [ ] **Step 4: Check the refactor contract**

```bash
git diff main --stat
git diff main -U0 | grep -c '^-.*UE_LOG('
git diff main -U0 | grep -c '^+.*UE_LOG('
```

Every removed `UE_LOG` must have a matching addition or a sentence in the PR saying which was dropped and why. Do the same eyeball for comment lines in the touched files — two refactors this year stripped a dozen justifications from members that never moved.

- [ ] **Step 5: Commit the spec amendment and open the PR**

```bash
git add docs/superpowers/specs/2026-09-21-route-policy-design.md
git commit -m "docs(spec): record the measured runway penalty

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push -u origin feature/route-policy
gh pr create --base main --title "One route policy: ERouteErrand and FRoutePolicy" --body "..."
```

The PR body fills in the project template: the build line, the test line (`N test(s) run, N failed, N crashed`), and for the refactor portion the log-line and comment-line deltas from Step 4. Attach `after-fix.png`.

End the body with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Self-Review

**Spec coverage.** §2 decisions 1-5 → Tasks 1, 3, 4, 6. §3 `ERouteErrand` → Task 1. §4 `FRoutePolicy` and the table → Task 1. §5 cost function → Task 4; §5.1 the memo → Task 2. §6 enforcement → Task 6 (refusals) and Task 7 (lint). §7 migration, all ten rows → Task 5; §7.1 `FindRoute` → Task 5 Steps 6-7; §7.2 test migration → Task 6 Step 3. §8 tests → every task's own steps, with the memo counter in Task 2 and the both-ways refusal in Task 6. §9 the §4 amendment → **gap, now closed:** Task 8 Step 3 amends §10 of the spec but nothing amended the ground-traffic spec's §4. Add to Task 8 Step 5: also apply the §9 replacement paragraph to `docs/superpowers/specs/2026-09-06-ground-traffic-design.md` and include it in that commit. §10 risks → Task 8 Steps 1-4. §11 out of scope → nothing to do.

**Placeholder scan.** One deliberate `--body "..."` in Task 8, immediately followed by the sentence saying what goes in it. The `AirsideTestWorld.h` stub in Task 5 Step 6 says "just widen the signature" and shows the signature. No TBDs.

**Type consistency.** `FRoutePolicy` fields `Avoidance`/`Occupancy`/`bPenaliseRunways` used identically in Tasks 1, 3, 4, 5, 6. `FRouteQuery::Errand`/`Policy`/`RunwayPenalty` introduced in Task 3, used in 4, 5, 6. `EdgeCost`'s new `bool bRunwayEdge` added and called in the same task. `RunwaySeedResolveCountForTest` declared and defined in Task 2, used only there. `FindRoute`'s `ERouteErrand Errand = ERouteErrand::PlayerIssued` is the same spelling in all four declarations and both definitions. `EOccupancyUse::Required`/`Never` consistent throughout.

**One correction made inline:** Task 3 Step 3 originally proposed forward-declaring `FRoutePolicy` in `RouteSearch.h`, which UHT cannot do for a by-value `USTRUCT` member. Resolved by moving `ERunwayAvoidance` down into `RoutePolicy.h` and having `RouteSearch.h` include it — the dependency runs the way it should, and the step now states that outright rather than leaving the implementer to discover it.
