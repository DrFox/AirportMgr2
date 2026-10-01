#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAllocator.h"
#include "Solve/IcaoCode.h"

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

	/** The Index-th entity NetworkWithStands placed. */
	FEntityInstanceId StandAt(const URoadNetwork& Network, int32 Index)
	{
		return Network.EntityIdAt(Index);
	}
}

// NO "TakesTheSmallestThatFits" ANY MORE (#471): it pinned UStandAllocator::Reserve's own smallest-fit walk, which went
// with its last caller - every re-hold is a plan's stand now (UFlightBoard::Rehold). Smallest-fit is ArrivalPlanner::
// ChooseStand's rule, pinned there (Airside.Model.StandChoice.SmallestLetterBeatsNearer, .FallsThroughToBigger). The
// tests below were Reserve's too, and pin what Hold asks of the ONE stand it is handed - the checks Reserve asked of
// every stand it walked, which Hold kept.

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

	TestFalse(TEXT("a stand that does not admit the wingspan is refused"),
		Allocator->Hold(*Traffic, *Network, *Wide, StandAt(*Network, 0)));
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
		Allocator->Hold(*Traffic, *Network, *Flight, StandAt(*Network, 0)));
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

	const FEntityInstanceId Only = StandAt(*Network, 0);
	TestTrue(TEXT("the first flight gets the only stand"),
		Allocator->Hold(*Traffic, *Network, *First, Only));
	TestFalse(TEXT("the second is refused rather than given the same stand"),
		Allocator->Hold(*Traffic, *Network, *Second, Only));

	Allocator->Release(*Traffic, *First);
	TestTrue(TEXT("and gets it once the first lets go"),
		Allocator->Hold(*Traffic, *Network, *Second, Only));
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
	TestTrue(TEXT("held before the edit"), Allocator->Hold(*Traffic, *Network, *Flight, StandAt(*Network, 0)));

	// GroundTrafficRebuild drops EVERY node claim - "a set of resources ceasing to exist" - and
	// until PR D's review (I1) only a LOAD re-held the flights' stands through Reapply, so the
	// player editing a taxiway silently un-reserved every stand and the next flight was handed
	// one already spoken for. The traffic model now re-holds its own reservations through its
	// own rebuild, by stand entity; Reapply stays the load's path and must agree with it.
	Traffic->OnGraphRebuilt(*Network);
	TestEqual(TEXT("the rebuild itself keeps the hold, under the flight's holder"),
		Traffic->HolderOfNode(Network->GetEntity(Flight->Stand)->PoseNode), Flight->HolderId());

	Allocator->Reapply(*Traffic, *Network, {Flight});
	TestEqual(TEXT("and a Reapply on top (the load's path) leaves it the flight's"),
		Traffic->HolderOfNode(Network->GetEntity(Flight->Stand)->PoseNode), Flight->HolderId());

	UFlight* Rival = FlightNeeding(3400.0, 2);
	TestFalse(TEXT("the reservation keeps a rival off the stand after a rebuild"),
		Allocator->Hold(*Traffic, *Network, *Rival, Flight->Stand));
	// #442: AND THE TABLE AGREES WITH THE COPY - HoldIsLost, the board's test, says nothing was lost.
	TestFalse(TEXT("the flight's Stand is one the table holds for it - not lost"), UStandAllocator::HoldIsLost(*Flight, *Traffic, *Network));
	TestTrue(TEXT("CONTROL: the rival, pointed at the same stand without a hold, reads as lost"),
		[&]() { Rival->Stand = Flight->Stand; return UStandAllocator::HoldIsLost(*Rival, *Traffic, *Network); }());
	return true;
}

// NAMED, NOT ANONYMOUS, unlike the helpers at the top of this file: the tests module is a
// UNITY build, and a new anonymous helper is one more name that can collide with another
// file's copy the moment both land in one translation unit.
namespace StandAllocatorDepotTest
{
	/** A fuel depot placed the raw way - DesignWingspan 0, which is exactly the "unknown,
	 *  admits anything" IcaoCode::StandAdmits reads, and a pose node, which is all the old
	 *  filter asked for. */
	void PlaceDepot(URoadNetwork& Net, const FVector2D& At)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Net.PlaceEntity(Depot, Depot->Anchors, At, 0.0, /*DesignWingspan*/ 0.0,
			Depot->PoseRole, Depot->Trucks);
	}
}

// FINAL REVIEW C1: bcc0b05 moved this allocator onto IcaoCode::StandAdmits/StandRank, under
// which a 0 span admits anything and ranks as C - and a fuel depot's DesignWingspan is 0. The
// old filter was alive + pose node only, never IsStand(), so a depot became the "smallest"
// stand for every flight up to Code C and a candidate for everything wider. ChooseStand had
// the kind check all along; the two now call ONE predicate, FEntityInstance::IsStandCandidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorNeverReservesADepotTest,
	"AirportOps.Model.StandAllocator.NeverReservesADepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorNeverReservesADepotTest::RunTest(const FString& Parameters)
{
	using namespace StandAllocatorDepotTest;

	// THE DEPOT FIRST, so enumeration order cannot be what saves the stand: a depot ranking C
	// beats an F stand on rank alone, whatever order they sit in.
	{
		URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
		PlaceDepot(*Network, FVector2D(-20000.0, 0.0));
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 0.0), 0.0,
			IcaoCode::DesignSpanForLetter(EIcaoCode::F), Stand->PoseRole, Stand->Trucks);

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
		UStandAllocator* Allocator = NewObject<UStandAllocator>();
		UFlight* A380 = FlightNeeding(7980.0, 1);   // 79.8 m, the A380-800's published span
		TestFalse(TEXT("an A380 flight is never held a fuel depot, whose span 0 'admits anything'"),
			Allocator->Hold(*Traffic, *Network, *A380, StandAt(*Network, 0)));
		if (!TestTrue(TEXT("CONTROL: the F stand beside it is held"), Allocator->Hold(*Traffic, *Network, *A380, StandAt(*Network, 1))))
		{
			return false;
		}
		const FEntityInstance* Chosen = Network->GetEntity(A380->Stand);
		TestTrue(TEXT("and it is the F stand - a fuel depot is never a stand to hold"),
			Chosen != nullptr && Chosen->IsStand());
	}

	// EVERY C STAND HELD: the depot is still no fallback. A flight with nowhere to go is
	// refused, which is the answer the sequencer can act on; one "parked" in a fuel depot is
	// an aeroplane sent to taxi into a truck yard.
	{
		URoadNetwork* Network = NetworkWithStands({ IcaoCode::DesignSpanForLetter(EIcaoCode::C) });
		PlaceDepot(*Network, FVector2D(-20000.0, 0.0));
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
		UStandAllocator* Allocator = NewObject<UStandAllocator>();
		UFlight* First = FlightNeeding(3580.0, 1);
		UFlight* Second = FlightNeeding(3580.0, 2);
		if (!TestTrue(TEXT("the first C flight takes the only C stand"), Allocator->Hold(*Traffic, *Network, *First, StandAt(*Network, 0))))
		{
			return false;
		}
		TestFalse(TEXT("the second C flight is refused rather than handed the depot"),
			Allocator->Hold(*Traffic, *Network, *Second, StandAt(*Network, 1)));
		TestFalse(TEXT("and nothing was written to it"), Second->Stand.IsSet());
	}
	return true;
}

// STAND ADMISSION'S RULE, NOW THIS ALLOCATOR'S TOO (shared-pavement Task 9): admission moved
// from the raw IcaoCode::StandAdmits size check to StandAdmission::Judge, the one rule
// ArrivalPlanner::ChooseStand also calls - so a stand this allocator holds for a flight is
// never one ChooseStand would refuse the same aircraft at touchdown for its pavement either,
// not only its letter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorSkipsAGrassStandForATarmacFlightTest,
	"AirportOps.Model.StandAllocator.SkipsAGrassStandForATarmacFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorSkipsAGrassStandForATarmacFlightTest::RunTest(const FString& Parameters)
{
	// TWO STANDS OF THE SAME SIZE, so a rank tie would first-found-win the grass one if
	// admission still read size alone - the fixing edit is what stops that.
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId GrassId = Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 0.0), 0.0,
		3600.0, Stand->PoseRole, Stand->Trucks);
	const FEntityInstanceId TarmacId = Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 0.0), 0.0,
		3600.0, Stand->PoseRole, Stand->Trucks);
	FRoadNetworkTestAccess Access(*Network);
	Access.SetEntityPavementForTest(GrassId, EPavement::Grass);
	Access.SetEntityPavementForTest(TarmacId, EPavement::Tarmac);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Flight = FlightNeeding(3400.0, 1);
	Flight->Airframe.MinimumPavement = EPavement::Tarmac;

	TestFalse(TEXT("a flight needing tarmac is never held the grass stand of its own size"),
		Allocator->Hold(*Traffic, *Network, *Flight, GrassId));
	if (!TestTrue(TEXT("and is held the tarmac one"),
		Allocator->Hold(*Traffic, *Network, *Flight, TarmacId))) { return false; }
	const FEntityInstance* Chosen = Network->GetEntity(Flight->Stand);
	TestTrue(TEXT("the tarmac stand, not the grass one of its own size"),
		Chosen != nullptr && Chosen->Pavement == EPavement::Tarmac);
	return true;
}

#endif
