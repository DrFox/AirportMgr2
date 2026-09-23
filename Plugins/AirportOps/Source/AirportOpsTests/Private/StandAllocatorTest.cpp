#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** One stand per wingspan, spaced so their pose nodes differ. */
	URoadNetwork* NetworkWithStands(const TArray<double>& Wingspans)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		double X = 0.0;
		for (const double Wingspan : Wingspans)
		{
			// The design wingspan is PlaceEntity's OWN argument, so one definition stands in
			// for stands of several sizes. A UEntityDefinition per size would test nothing
			// extra and would drift from the content the game ships.
			Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(X, 0.0), 0.0, Wingspan,
				Stand->PoseRole, Stand->Trucks);
			X += 20000.0;
		}
		return Net;
	}

	UFlight* FlightNeeding(double Wingspan, int32 Id)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Id = Id;
		Flight->Airframe.Wingspan = Wingspan;
		return Flight;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorSmallestFitTest,
	"AirportOps.Model.StandAllocator.TakesTheSmallestThatFits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorSmallestFitTest::RunTest(const FString& Parameters)
{
	// The 3600 stand is placed SECOND, so first-fit-by-order would take the 6000 one. The
	// rule is smallest that admits, because the big stand is the scarce thing.
	URoadNetwork* Network = NetworkWithStands({6000.0, 3600.0, 5200.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Flight = FlightNeeding(3400.0, 1);

	TestTrue(TEXT("a fitting stand is reserved"), Allocator->Reserve(*Traffic, *Network, *Flight));

	const FEntityInstance* Chosen = Network->GetEntity(Flight->Stand);
	TestNotNull(TEXT("the reservation names a live stand"), Chosen);
	TestEqual(TEXT("the SMALLEST stand that admits it, not the first"),
		Chosen != nullptr ? Chosen->DesignWingspan : 0.0, 3600.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorTooWideTest,
	"AirportOps.Model.StandAllocator.RefusesWhatNoStandAdmits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorTooWideTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Wide = FlightNeeding(6500.0, 1);

	TestFalse(TEXT("a wingspan no stand admits is refused"),
		Allocator->Reserve(*Traffic, *Network, *Wide));
	TestFalse(TEXT("and nothing was written to the flight"), Wide->Stand.IsSet());
	return true;
}

// FIX ROUND 1 (drawn-stands Task 4 review, finding 2): this allocator used to admit by RAW
// DesignWingspan compare (Stand.DesignWingspan < Wingspan), which disagreed with
// ArrivalPlanner::ChooseStand's letter-based admission on exactly this pair - a legacy stand
// captured at 3410 (an older A320 measurement) is Code C, same as a 737-800's published 3580,
// so ChooseStand admitted it while this allocator refused it (3410 < 3580 as raw doubles).
// Both now call IcaoCode::StandAdmits/StandRank, so a stand this allocator holds for a flight
// is never one ChooseStand would refuse the same aircraft at touchdown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorAgreesWithChooseStandOnLegacySpansTest,
	"AirportOps.Model.StandAllocator.AgreesWithChooseStandOnLegacySpans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorAgreesWithChooseStandOnLegacySpansTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3410.0}); // legacy Code C figure, not today's 3580
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();

	UAircraftType* B738 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(B738);
	UFlight* Flight = FlightNeeding(B738->Airframe().Wingspan, 1); // 3580, also Code C

	TestTrue(TEXT("a legacy-span stand still admits by letter, agreeing with ChooseStand - not by raw span"),
		Allocator->Reserve(*Traffic, *Network, *Flight));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorNeverDoubleBooksTest,
	"AirportOps.Model.StandAllocator.NeverDoubleBooks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorNeverDoubleBooksTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* First = FlightNeeding(3400.0, 1);
	UFlight* Second = FlightNeeding(3400.0, 2);

	TestTrue(TEXT("the first flight gets the only stand"),
		Allocator->Reserve(*Traffic, *Network, *First));
	TestFalse(TEXT("the second is refused rather than given the same stand"),
		Allocator->Reserve(*Traffic, *Network, *Second));

	Allocator->Release(*Traffic, *First);
	TestTrue(TEXT("and gets it once the first lets go"),
		Allocator->Reserve(*Traffic, *Network, *Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorSurvivesRebuildTest,
	"AirportOps.Model.StandAllocator.SurvivesAGraphRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorSurvivesRebuildTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Flight = FlightNeeding(3400.0, 1);
	TestTrue(TEXT("reserved before the edit"), Allocator->Reserve(*Traffic, *Network, *Flight));

	// GroundTrafficRebuild drops EVERY node claim - "a set of resources ceasing to exist" -
	// so without Reapply the player editing a taxiway silently un-reserves every stand and
	// the next flight is handed one that is already spoken for.
	Traffic->OnGraphRebuilt(*Network);
	TestFalse(TEXT("the rebuild really did drop the hold, or this test proves nothing"),
		Traffic->IsStandHeld(Network->GetEntity(Flight->Stand)->PoseNode, 0));

	Allocator->Reapply(*Traffic, *Network, {Flight});

	UFlight* Rival = FlightNeeding(3400.0, 2);
	TestFalse(TEXT("the reservation keeps a rival off the stand after a rebuild"),
		Allocator->Reserve(*Traffic, *Network, *Rival));
	return true;
}

#endif
