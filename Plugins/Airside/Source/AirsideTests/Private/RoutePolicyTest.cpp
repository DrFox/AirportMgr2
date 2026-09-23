#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "Model/Airframe.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficRules.h"

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

		// GraphProbe is the ONLY errand allowed to be fully permissive, and
		// DepartureBacktrack the only one whose whole purpose is the strip. Every other row
		// must either keep aircraft off a runway or charge them for it; a row that does
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
	// policy to preserve. See the spec's section 7.
	auto Row = [this](ERouteErrand Errand, ERunwayAvoidance Avoidance, EOccupancyUse Occupancy, const TCHAR* Site)
	{
		const FRoutePolicy Policy = FRoutePolicy::For(Errand);
		TestEqual(*FString::Printf(TEXT("%s kept its runway avoidance"), Site), Policy.Avoidance, Avoidance);
		TestEqual(*FString::Printf(TEXT("%s kept its occupancy use"), Site), Policy.Occupancy, Occupancy);
	};

	Row(ERouteErrand::ArrivalTaxiIn,       ERunwayAvoidance::All,  EOccupancyUse::Never,    TEXT("ArrivalPlanner.cpp:36"));
	Row(ERouteErrand::DepartureToEntry,    ERunwayAvoidance::All,  EOccupancyUse::Never,    TEXT("DeparturePlanner.cpp:88"));
	Row(ERouteErrand::DepartureBacktrack,  ERunwayAvoidance::None, EOccupancyUse::Never,    TEXT("DeparturePlanner.cpp:106"));
	Row(ERouteErrand::Replan,              ERunwayAvoidance::Held, EOccupancyUse::Required, TEXT("GroundTrafficRebuild.cpp:95"));
	Row(ERouteErrand::CandidateComparison, ERunwayAvoidance::All,  EOccupancyUse::Never,    TEXT("FuelService.cpp:234"));

	return true;
}

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
		Airframe.Wingspan, ETraversalClass::Aircraft);

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
		Airframe.Wingspan, ETraversalClass::Aircraft);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteErrandRefusalTest,
	"Airside.Model.RoutePolicy.SearchRefusesABadQuery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteErrandRefusalTest::RunTest(const FString& Parameters)
{
	// THESE ARE THE ASSERTIONS THAT THE GUARDS LOG, not a way of silencing them.
	//
	// UE's automation controller promotes an Error line to a test failure - which is exactly
	// why the guards log at Error rather than Warning, a Warning being invisible to a test
	// run in this project. AddExpectedError matches BOTH ways: an expectation that never
	// matches fails the test, and an Error that no expectation covers fails it too. So the
	// counts below assert that each refusal fired, with its wording, exactly this many times.
	//
	// AN FOutputDevice SPY CANNOT BE USED HERE, and that was the first attempt: the framework
	// consumes a matched Error before any output device sees it, so the spy read zero every
	// time while the refusals were firing perfectly well.
	AddExpectedError(TEXT("Route query has no errand"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("requires the occupancy table"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("must not read the occupancy table"), EAutomationExpectedErrorFlags::Contains, 1);

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 1000.0, 0.0);
	TestGraph::Join(*Net, A, B);

	const FAirframe Airframe;

	// THE CONTROL FIRST: a properly-named errand DOES route over this graph, so every
	// refusal below is about the query and not about an empty network.
	{
		const FRoutePlan Good = RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Airframe.Wingspan, ETraversalClass::Aircraft));
		if (!TestTrue(TEXT("the control routes, so the refusals below are about the query"), Good.IsValid()))
		{
			return false;
		}
	}

	// 1. No errand at all - the shape the four undeclared call sites had.
	{
		FRouteQuery Q;
		Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
		const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
		TestFalse(TEXT("a query with no errand does not route"), Plan.IsValid());
	}

	// 2. Required occupancy, no table.
	{
		const FRouteQuery Q = FRouteQuery::For(ERouteErrand::VehicleToJob, A, B, Airframe.Wingspan, ETraversalClass::GroundVehicle);
		TestFalse(TEXT("an errand that must weigh congestion will not route without the table"),
			RouteSearch::Find(*Net, Q).IsValid());
	}

	// 3. Never occupancy, table supplied anyway. THE HALF THAT WOULD ROT if only the first
	// were written: a caller that supplied a table believes it is being weighted by it, and
	// silently dropping the pointer leaves it reasoning about a cost never applied.
	{
		FTrafficOccupancy Table;
		FRouteQuery Q = FRouteQuery::For(ERouteErrand::CandidateComparison, A, B, Airframe.Wingspan, ETraversalClass::GroundVehicle);
		Q.WithCongestion(Table, 1, 2.0);
		TestFalse(TEXT("an errand costed on shape alone refuses a table rather than ignoring it"),
			RouteSearch::Find(*Net, Q).IsValid());
	}

	// 4. FindToGoals guards identically. A guard on one of two entry points is the
	// "check where a list is CONSUMED" bug wearing a new hat.
	{
		FRouteQuery Q;
		Q.Start = A; Q.Class = ETraversalClass::Aircraft;
		TArray<FGoalReach> Reach;
		const FMultiGoalSearch Search = RouteSearch::FindToGoals(*Net, Q, { B }, Reach);
		TestEqual(TEXT("FindToGoals settles nothing on a query with no errand"), Search.Arrived.Num(), 0);
		TestEqual(TEXT("but OutReach is still sized, so a caller indexing it does not read past the end"),
			Reach.Num(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
