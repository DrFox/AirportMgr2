#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ERunwayBusy::Queue (spec 2026-09-28-arrival-queue section 1): accepting an offer is about the
 * STAND. A busy runway is something to queue for, so the accept asks Plan to skip the runway
 * occupancy step - and must still reach every step after it, the stand above all.
 */
namespace
{
	/** Every surface of the test airport's runway, held by agent 99 - a landing rolling out. */
	void HoldRunway(const FTestAirport& Airport, FTrafficOccupancy& Occupancy)
	{
		for (const FTrafficResource& Surface : Airport.Net->RunwaySurfaces(Airport.ThresholdSegment))
		{
			FTrafficClaim Claim;
			Claim.AgentId = 99;
			Claim.Resource = Surface;
			Claim.bOccupied = true;
			FTrafficClaim Blocker;
			Occupancy.TryClaim(Claim, Blocker);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueIgnoresBusyRunwayTest, "Airside.Model.ArrivalQueue.QueueIgnoresABusyRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueIgnoresBusyRunwayTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe);
	FTrafficOccupancy Occupancy;
	HoldRunway(Airport, Occupancy);

	TestEqual(TEXT("a landing now is refused while the strip is held"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy).Why,
		EArrivalRefusal::RunwayOccupied);
	TestEqual(TEXT("but a flight that will queue for it is fine"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy, ERunwayBusy::Queue).Why,
		EArrivalRefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueStillChecksStandsTest, "Airside.Model.ArrivalQueue.QueueStillChecksStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueStillChecksStandsTest::RunTest(const FString& Parameters)
{
	// THE REASON THIS IS A PLAN OPTION and not a RunwayOccupied-means-fine mapping after the
	// fact: Plan returns at the runway step BEFORE the stand steps, so a busy runway would hide
	// a genuinely full stand row and the player would accept a flight with nowhere to park.
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe);
	if (!TestTrue(TEXT("the fixture has a stand"), Airport.Stands.Num() > 0)) { return false; }
	FTrafficOccupancy Occupancy;
	HoldRunway(Airport, Occupancy);
	for (const FEntityInstanceId Stand : Airport.Stands)
	{
		const FEntityInstance* Instance = Airport.Net->GetEntity(Stand);
		if (Instance != nullptr)
		{
			FTrafficClaim Claim;
			Claim.AgentId = -7;
			Claim.Resource = FTrafficResource::OfNode(Instance->PoseNode);
			Claim.bOccupied = true;
			FTrafficClaim Blocker;
			Occupancy.TryClaim(Claim, Blocker);
		}
	}
	TestEqual(TEXT("with every stand taken, queueing for the runway still says no stand"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy, ERunwayBusy::Queue).Why,
		EArrivalRefusal::NoFreeStand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueRunwayBusyAgreesTest, "Airside.Model.ArrivalQueue.IsRunwayBusyAgreesWithPlan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueRunwayBusyAgreesTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe);
	FTrafficOccupancy Occupancy;
	TestFalse(TEXT("an empty runway is not busy"), ArrivalPlanner::IsRunwayBusy(*Airport.Net, Airport.Threshold, &Occupancy));
	TestFalse(TEXT("nor is one with no occupancy to ask"), ArrivalPlanner::IsRunwayBusy(*Airport.Net, Airport.Threshold, nullptr));
	HoldRunway(Airport, Occupancy);
	TestTrue(TEXT("a held one is"), ArrivalPlanner::IsRunwayBusy(*Airport.Net, Airport.Threshold, &Occupancy));
	TestEqual(TEXT("and Plan refuses on exactly that"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy).Why, EArrivalRefusal::RunwayOccupied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueOwnHoldTest, "Airside.Model.ArrivalQueue.PlanExcludesItsOwnHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueOwnHoldTest::RunTest(const FString& Parameters)
{
	// A HOLDING FLIGHT KEEPS ITS STAND, so asking "can it land now" must not be refused by its
	// OWN hold (review C1, 2026-09-28) - the queue asks the whole plan, not just the runway.
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe);
	if (!TestTrue(TEXT("the fixture has a stand"), Airport.Stands.Num() == 1)) { return false; }
	FTrafficOccupancy Occupancy;
	const FEntityInstance* Stand = Airport.Net->GetEntity(Airport.Stands[0]);
	FTrafficClaim Claim;
	Claim.AgentId = -7;
	Claim.Resource = FTrafficResource::OfNode(Stand->PoseNode);
	Claim.bOccupied = true;
	FTrafficClaim Blocker;
	Occupancy.TryClaim(Claim, Blocker);
	TestEqual(TEXT("someone else's hold is a full stand row"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy).Why, EArrivalRefusal::NoFreeStand);
	TestEqual(TEXT("its own hold is not"),
		ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe, &Occupancy, ERunwayBusy::Refuse, -7).Why,
		EArrivalRefusal::None);
	return true;
}

#endif
