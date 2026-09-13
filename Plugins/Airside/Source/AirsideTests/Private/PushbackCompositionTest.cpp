#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/LandingRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentPushbackCompositionTest,
	"Airside.Present.AgentPushbackComposition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentPushbackCompositionTest::RunTest(const FString& Parameters)
{
	// AT THE LEVEL OF THE COMPOSITION, not the model struct. Every other pushback test drives
	// FPushbackRun or FRoadAgent with no world at all; this one spawns the actor, ticks it,
	// and watches a real aeroplane actually leave a real stand.
	//
	// IT IS THE ONE THAT FAILS IF THE PHASE IS UNWIRED. A manoeuvre wired into the model but
	// never reached from the driver - or reached, but with the view never posed during it -
	// passes every world-free assertion in this branch and shows a frozen aeroplane on screen.
	// That is the gap ArrivalDispatchTest's own header describes: "FLandingRun passed in
	// isolation and RunwayExitNodes passed in isolation, and pressing 7 did nothing."
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World))
	{
		return false;
	}
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	// THE SEAM AirportOps DRIVES ON. Recorded through the actor's own delegate, because a
	// transition observed on FRoadAgent proves nothing about whether UAirsideTraffic::Advance
	// relays it - which is the layer the flight board listens to.
	TArray<TPair<EAgentPhase, EAgentPhase>> Transitions;
	Actor->GetTraffic()->OnAgentPhaseChanged.AddLambda(
		[&Transitions](int32, EAgentPhase From, EAgentPhase To) { Transitions.Emplace(From, To); });

	FAirframe Airframe;
	Airframe.Ground = TestAirframes::Piper().Ground;
	Airframe.Climb = TestAirframes::Piper().Climb;
	Airframe.Approach = TestAirframes::Piper().Approach;
	Airframe.Engine = TestAirframes::Piper().Engine;

	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get()))
	{
		return false;
	}

	// The same airport ArrivalDispatchTest builds, and for its reasons: onto the actor's OWN
	// network, no stand from the fixture (Actor->PlaceStand needs the content set's authored
	// definition), and NOT derived here - Actor->RebuildMesh does that, which is the real path
	// a road edit takes.
	const FTestAirport Fixture =
		FTestAirport::Build(Airframe, { .StandCount = 0, .bDerived = false }, Actor->Network.Get());
	const FVector2D ThresholdAt = Fixture.Threshold;
	const FVector2D ExitAt = Fixture.ExitAt;

	// Beside the taxiway and facing it, so FAnchorLink has a ray to cast - the arrangement
	// Airside.Build.LeadInSweep uses. Heading is RADIANS throughout PlaceEntity.
	const FVector2D StandAt = ExitAt + FVector2D(9000.0, -10000.0);

	Actor->RebuildMesh();

	if (Actor->ResolveStandDefinitionForTest() == nullptr)
	{
		// Reported rather than skipped silently: a test that quietly asserts nothing is worse
		// than one that says why.
		AddInfo(TEXT("No stand definition available in this run - pushback not exercised."));
		return true;
	}

	if (!TestTrue(TEXT("a stand is placed at the end of the taxiway"),
		Actor->PlaceStand(StandAt, 0.0) != INDEX_NONE))
	{
		return false;
	}
	Actor->RebuildMesh();

	if (!TestTrue(TEXT("an arrival is accepted"), Actor->DispatchArrival(ThresholdAt, Airframe)))
	{
		return false;
	}
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	// Land and taxi in, the shape ArrivalDispatchTest uses for the same wait.
	for (int32 Ticks = 0;
		Ticks < 6000 && Actor->LastAgentPhaseForTest() != EAgentPhase::Parked; ++Ticks)
	{
		Actor->Tick(0.1f);
	}
	if (!TestEqual(TEXT("the arrival parks"),
		Actor->LastAgentPhaseForTest(), EAgentPhase::Parked))
	{
		return false;
	}

	const AActor* View = Actor->GetNewestAgent();
	if (!TestNotNull(TEXT("the aeroplane has a view"), View))
	{
		return false;
	}
	const FVector ParkedAt = View->GetActorLocation();

	Transitions.Reset();

	const EDepartureRefusal Why = Actor->DepartAgent(Id);
	if (!TestEqual(FString::Printf(TEXT("it is cleared to leave (%d)"), static_cast<int32>(Why)),
		Why, EDepartureRefusal::None))
	{
		return false;
	}

	// THE VIEW IS POSED ON EVERY FRAME OF THE MANOEUVRE. This project's recurring failure is
	// an actor left at the world origin for a frame, which is the whole reason
	// FRoadAgent::LastMotion exists - and a push is a phase nothing had ever posed before.
	bool bEverAtOrigin = false;
	double FurthestFromStand = 0.0;
	int32 ManoeuvringTicks = 0;

	for (int32 Ticks = 0;
		Ticks < 6000 && Actor->LastAgentPhaseForTest() == EAgentPhase::Manoeuvring; ++Ticks)
	{
		Actor->Tick(0.1f);
		++ManoeuvringTicks;

		if (const AActor* Now = Actor->GetNewestAgent())
		{
			const FVector At = Now->GetActorLocation();
			bEverAtOrigin = bEverAtOrigin || At.SizeSquared() < 1.0;
			FurthestFromStand = FMath::Max(FurthestFromStand, FVector::Dist2D(At, ParkedAt));
		}
	}

	TestTrue(TEXT("the aeroplane spent real time manoeuvring"), ManoeuvringTicks > 0);
	TestFalse(TEXT("and no frame of it put the view at the world origin"), bEverAtOrigin);

	// IT ACTUALLY MOVED, rather than merely changing phase. A manoeuvre that advanced the
	// model without moving the actor would satisfy every assertion above.
	TestTrue(FString::Printf(TEXT("and it left the stand (%.0f uu)"), FurthestFromStand),
		FurthestFromStand > 500.0);

	// AND THE TRANSITION REACHED THE LAYER AirportOps LISTENS TO, in the right order. Asserted
	// as a SEQUENCE rather than a set: Parked -> Taxiing directly is exactly the defect this
	// feature removes, and a set would accept it alongside the manoeuvre.
	int32 ParkedToManoeuvring = INDEX_NONE;
	int32 ManoeuvringToTaxiing = INDEX_NONE;
	for (int32 Index = 0; Index < Transitions.Num(); ++Index)
	{
		if (Transitions[Index].Key == EAgentPhase::Parked
			&& Transitions[Index].Value == EAgentPhase::Manoeuvring)
		{
			ParkedToManoeuvring = Index;
		}
		if (Transitions[Index].Key == EAgentPhase::Manoeuvring
			&& Transitions[Index].Value == EAgentPhase::Taxiing)
		{
			ManoeuvringToTaxiing = Index;
		}
	}

	TestTrue(TEXT("leaving the stand is announced as Parked -> Manoeuvring"),
		ParkedToManoeuvring != INDEX_NONE);
	TestTrue(TEXT("and the taxi out as Manoeuvring -> Taxiing"),
		ManoeuvringToTaxiing != INDEX_NONE);
	TestTrue(TEXT("in that order"),
		ParkedToManoeuvring != INDEX_NONE && ManoeuvringToTaxiing > ParkedToManoeuvring);

	TestFalse(TEXT("and never straight from Parked to Taxiing, which is the defect"),
		Transitions.ContainsByPredicate([](const TPair<EAgentPhase, EAgentPhase>& T)
			{ return T.Key == EAgentPhase::Parked && T.Value == EAgentPhase::Taxiing; }));

	return true;
}

#endif
