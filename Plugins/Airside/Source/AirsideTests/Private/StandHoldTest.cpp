#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A bare handle. FGuidelineNodeId has no int constructor - the fields are set by hand. */
	FGuidelineNodeId NodeAt(int32 Index)
	{
		FGuidelineNodeId Out;
		Out.Index = Index;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandHoldReservesTest,
	"Airside.Traffic.StandHoldReserves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandHoldReservesTest::RunTest(const FString& Parameters)
{
	// THE POINT: a stand can be held by something that is not an agent - an accepted flight,
	// hours before it is dispatched - and the hold is the same claim ArrivalPlanner already
	// honours, so nothing else has to learn about reservations.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	const FGuidelineNodeId Stand = NodeAt(7);

	TestTrue(TEXT("a free stand is held once asked for"), Traffic->HoldStand(-1, Stand));
	TestTrue(TEXT("the hold is visible to anyone but the holder"), Traffic->IsStandHeld(Stand, 0));
	TestFalse(TEXT("the holder does not see its own hold"), Traffic->IsStandHeld(Stand, -1));
	TestFalse(TEXT("a second holder is refused the same stand"), Traffic->HoldStand(-2, Stand));

	Traffic->ReleaseHold(-1);
	TestFalse(TEXT("releasing frees it"), Traffic->IsStandHeld(Stand, 0));
	TestTrue(TEXT("and the next holder gets it"), Traffic->HoldStand(-2, Stand));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandHoldIsNotABodyTest,
	"Airside.Traffic.StandHoldIsNotABody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandHoldIsNotABodyTest::RunTest(const FString& Parameters)
{
	// A hold is a RESERVATION, never an occupation: nothing's body is at a stand hours before
	// it lands. The distinction is what lets ReleaseHold use ReleaseReservations, which
	// refuses to take a claim that contains an agent's own position.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	const FGuidelineNodeId Stand = NodeAt(3);
	TestTrue(TEXT("held"), Traffic->HoldStand(-4, Stand));

	const TArray<FTrafficClaim>& Claims = Traffic->GetOccupancy().GetClaims();
	const FTrafficClaim* Held = Claims.FindByPredicate(
		[](const FTrafficClaim& C) { return C.AgentId == -4; });
	TestNotNull(TEXT("the claim reached the one occupancy table"), Held);
	if (Held != nullptr)
	{
		TestFalse(TEXT("and it is a reservation, not an occupation"), Held->bOccupied);
	}
	return true;
}

#endif
