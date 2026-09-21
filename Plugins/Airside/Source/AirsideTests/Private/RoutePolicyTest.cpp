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

#endif // WITH_DEV_AUTOMATION_TESTS
