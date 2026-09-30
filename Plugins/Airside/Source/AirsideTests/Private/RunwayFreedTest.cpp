#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TakeoffRun.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * OnRunwayFreed and OnStandsFreed (ops batch 3 §5): a runway or a stand that was held and is not now, found by a
 * DIFF at the end of UGroundTraffic::Advance (and OnGraphRebuilt) of the same predicate the arrival queue asks -
 * ArrivalPlanner::IsChainHeld, UGroundTraffic::IsStandHeld. Every way a runway frees is one of these cases, and
 * each fails if the diff misses it: AirportOps' arrival queue waits on this event, not on a per-frame poll.
 */
namespace RunwayFreedTest
{
	/** Every broadcast, as it came. Prefixed namespace: the test module is a unity build. */
	struct FRecorder
	{
		TArray<FRoadSegmentId> Runways;
		TArray<TArray<FGuidelineNodeId>> Stands;

		void Bind(UGroundTraffic& Traffic)
		{
			Traffic.OnRunwayFreed.AddLambda([this](FRoadSegmentId Seed) { Runways.Add(Seed); });
			Traffic.OnStandsFreed.AddLambda([this](const TArray<FGuidelineNodeId>& Poses) { Stands.Add(Poses); });
		}
	};

	/** FCrossingFixture's strip with an aircraft dispatched S -> N across it. */
	struct FCrossing
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		FCrossingFixture Field;
		int32 Plane = 0;

		bool Build()
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			Field = FCrossingFixture::Build(*Net);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Plane = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, Field.S, Field.N, ETraversalClass::Aircraft),
				TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
			return Plane > 0;
		}

		bool Held() const { return ArrivalPlanner::IsChainHeld(*Net, Field.Strip, &Traffic->GetOccupancy()); }

		/** Ticks at the substep (BarToBarCrossing's reason: one call, one claim pass) until Pred or Seconds. */
		template <typename P>
		bool TickUntil(double Seconds, P Pred)
		{
			const double Dt = Traffic->Rules.MaxSubstepSeconds;
			for (double T = 0.0; T < Seconds; T += Dt)
			{
				Traffic->Advance(Dt, Net);
				if (Pred())
				{
					return true;
				}
			}
			return false;
		}
	};
}

using RunwayFreedTest::FRecorder;
using RunwayFreedTest::FCrossing;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedTakeOffTest, "Airside.Model.Traffic.RunwayFreed.TakeOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedTakeOffTest::RunTest(const FString&)
{
	// DepartureReleasesWhenAirborne's field: a split runway and a taxiway ending on it, which arms a departure.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Near = Net->AddStraightSegment(RA, RM, Runway);
	Net->AddStraightSegment(RM, RB, Runway);
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Net->AddGuidelineEdge(MoveTemp(Edge));
	}
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRecorder Seen;
	Seen.Bind(*Traffic);
	const int32 Plane = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, A, B, ETraversalClass::Aircraft),
		TestAirframes::Piper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched, armed to depart"), Plane > 0 && Traffic->FindAgent(Plane)->bDepartureArmed)) { return false; }

	// EVERY HELD -> FREE TRANSITION THE PREDICATE SHOWS IS ONE EVENT, and no event lands on a tick that ends held.
	bool bWasHeld = false;
	int32 Transitions = 0;
	bool bEventWhileHeld = false;
	bool bRolled = false;
	int32 AtAirborneRelease = INDEX_NONE;
	for (double Clock = 0.0; Clock < 300.0 && Traffic->FindAgent(Plane) != nullptr; Clock += 0.05)
	{
		const int32 Before = Seen.Runways.Num();
		Traffic->Advance(0.05, Net);
		const bool bHeld = ArrivalPlanner::IsChainHeld(*Net, Near, &Traffic->GetOccupancy());
		bEventWhileHeld |= bHeld && Seen.Runways.Num() != Before;
		Transitions += bWasHeld && !bHeld;
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		bRolled |= P != nullptr && P->Phase == EAgentPhase::Departing;
		if (bWasHeld && !bHeld && bRolled && AtAirborneRelease == INDEX_NONE)
		{
			AtAirborneRelease = Seen.Runways.Num() - Before;
		}
		bWasHeld = bHeld;
	}
	TestTrue(TEXT("it rolled and took off"), bRolled && Traffic->FindAgent(Plane) == nullptr);
	TestFalse(TEXT("no event fires on a tick that ends with the strip still held"), bEventWhileHeld);
	TestTrue(TEXT("the take-off released the strip"), Transitions >= 1);
	TestEqual(TEXT("the take-off's release fires one event on its own tick"), AtAirborneRelease, 1);
	TestEqual(TEXT("one event per held -> free transition"), Seen.Runways.Num(), Transitions);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedCrossingTest, "Airside.Model.Traffic.RunwayFreed.CrossingClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedCrossingTest::RunTest(const FString&)
{
	// THE NO-REVISION CASE: a taxiing crossing is held through the per-tick claim pass, and clears with no phase
	// change and no OccupancyRevision bump - the case a revision-keyed cache under-reports, and the one the arrival
	// queue used to poll every frame for.
	FCrossing C;
	if (!TestTrue(TEXT("dispatched across the strip"), C.Build())) { return false; }
	FRecorder Seen;
	Seen.Bind(*C.Traffic);

	if (!TestTrue(TEXT("the crossing takes the strip"), C.TickUntil(120.0, [&C]() { return C.Held(); }))) { return false; }
	bool bEventWhileHeld = false;
	uint32 RevisionBefore = 0;
	const bool bCleared = C.TickUntil(120.0, [&]()
	{
		bEventWhileHeld |= C.Held() && Seen.Runways.Num() > 0;
		if (C.Held())
		{
			RevisionBefore = C.Traffic->OccupancyRevision();
			return false;
		}
		return true;
	});
	if (!TestTrue(TEXT("and gives it back"), bCleared)) { return false; }
	TestFalse(TEXT("nothing fires while the crossing holds the strip"), bEventWhileHeld);
	TestEqual(TEXT("the tick the crossing clears fires once"), Seen.Runways.Num(), 1);
	TestTrue(TEXT("naming the crossed strip"), Seen.Runways.Num() == 1
		&& C.Net->RunwaySurfaces(Seen.Runways[0]).Contains(FTrafficResource::OfSurface(C.Field.Strip)));
	TestEqual(TEXT("with no OccupancyRevision bump - why a diff, not the revision, is the signal"),
		C.Traffic->OccupancyRevision(), RevisionBefore);

	for (int32 Tick = 0; Tick < 60; ++Tick) { C.Traffic->Advance(0.05, C.Net); }
	TestEqual(TEXT("a free strip stays quiet"), Seen.Runways.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedDespawnTest, "Airside.Model.Traffic.RunwayFreed.DespawnOnRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedDespawnTest::RunTest(const FString&)
{
	// THE PLAYER'S UNSTICK: an aircraft retired standing on the strip. RetireAgent drops its claims and diffs AT
	// ONCE (review M1): a release outside Advance used to wait for the next Advance, and a paused game has none - the
	// queue's safety net then landed the flight with a false "no event covered it".
	FCrossing C;
	if (!TestTrue(TEXT("dispatched across the strip"), C.Build())) { return false; }
	FRecorder Seen;
	Seen.Bind(*C.Traffic);
	if (!TestTrue(TEXT("on the strip"), C.TickUntil(120.0, [&C]()
		{
			const FRoadAgent* P = C.Traffic->FindAgent(C.Plane);
			return P != nullptr && P->GetCrossingPhase() == ECrossingPhase::OnStrip && C.Held();
		}))) { return false; }
	TestEqual(TEXT("held: nothing yet"), Seen.Runways.Num(), 0);
	C.Traffic->RetireAgent(C.Plane);
	TestEqual(TEXT("the despawn frees the strip at once, with no Advance"), Seen.Runways.Num(), 1);
	C.Traffic->Advance(0.05, C.Net);
	TestEqual(TEXT("and the next Advance does not say it again"), Seen.Runways.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedClearAgentsTest, "Airside.Model.Traffic.RunwayFreed.ClearAgents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedClearAgentsTest::RunTest(const FString&)
{
	// EVERY AGENT CLEARED (a load) empties the table outside Advance - the same "diff at once" as RetireAgent.
	FCrossing C;
	if (!TestTrue(TEXT("dispatched across the strip"), C.Build())) { return false; }
	FRecorder Seen;
	Seen.Bind(*C.Traffic);
	if (!TestTrue(TEXT("the crossing takes the strip"), C.TickUntil(120.0, [&C]() { return C.Held(); }))) { return false; }
	C.Traffic->ClearAgents();
	TestEqual(TEXT("ClearAgents frees the strip at once"), Seen.Runways.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedDeletedTest, "Airside.Model.Traffic.RunwayFreed.DeletedRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedDeletedTest::RunTest(const FString&)
{
	// A DELETED RUNWAY IS NO LONGER BUSY - even with an aeroplane's claims still naming its dead segment, which
	// the next claim pass would drop. The rebuild re-reads the runways and reports the vanished one at once.
	FCrossing C;
	if (!TestTrue(TEXT("dispatched across the strip"), C.Build())) { return false; }
	FRecorder Seen;
	Seen.Bind(*C.Traffic);
	if (!TestTrue(TEXT("the crossing takes the strip"), C.TickUntil(120.0, [&C]() { return C.Held(); }))) { return false; }
	TestTrue(TEXT("the strip is deleted"), C.Net->RemoveSegment(C.Field.Strip));
	TestEqual(TEXT("nothing fires before the rebuild"), Seen.Runways.Num(), 0);
	C.Traffic->OnGraphRebuilt(*C.Net);
	TestEqual(TEXT("the rebuild reports the vanished runway freed, before any Advance"), Seen.Runways.Num(), 1);
	C.Traffic->Advance(0.05, C.Net);
	TestEqual(TEXT("and only once"), Seen.Runways.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedStandsTest, "Airside.Model.Traffic.RunwayFreed.StandsDiff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedStandsTest::RunTest(const FString&)
{
	FTestAirportOptions Options;
	Options.StandCount = 2;
	const FTestAirport Field = FTestAirport::Build(TestAirframes::Piper(), Options);
	URoadNetwork* Net = Field.Net;
	auto Pose = [Net, &Field](int32 Index) { return Net->GetEntity(Field.Stands[Index])->PoseNode; };
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRecorder Seen;
	Seen.Bind(*Traffic);

	// A HOLD (an accepted flight's): held, then released.
	TestTrue(TEXT("stand 0 held for a flight"), Traffic->HoldStand(-7, Pose(0)));
	for (int32 Tick = 0; Tick < 5; ++Tick) { Traffic->Advance(0.05, Net); }
	TestEqual(TEXT("nothing while it is held"), Seen.Stands.Num(), 0);
	Traffic->ReleaseHold(-7);
	// AT ONCE, not on the next Advance (review M1): a hold given back by a decline or a cancel is released between
	// ticks, and a paused game has no next Advance.
	if (TestEqual(TEXT("the release itself fires: one broadcast"), Seen.Stands.Num(), 1))
	{
		TestTrue(TEXT("naming stand 0 alone"), Seen.Stands[0].Num() == 1 && Seen.Stands[0][0] == Pose(0));
	}

	// AN AGENT'S GOAL, then the agent gone.
	// FROM THE TAXIWAY'S FAR END, not from stand 0: a body standing on stand 0 would hold it, and its leaving
	// would be a second freed stand this step is not about.
	const FGuidelineNodeId TaxiEnd = RouteSearch::FindNearestNode(*Net, Field.ExitAt + FVector2D(0.0, -20000.0),
		ETraversalClass::Aircraft, 5000.0);
	const FRoutePlan ToStand1 = TestGraph::Probe(*Net, TaxiEnd, Pose(1), ETraversalClass::Aircraft);
	const int32 Plane = ToStand1.IsValid() ? Traffic->DispatchAgent(Net, ToStand1, TestAirframes::Piper(), ETraversalClass::Aircraft, 1.0) : 0;
	if (!TestTrue(TEXT("an aircraft heads for stand 1"), Plane > 0)) { return false; }
	Traffic->Advance(0.05, Net);
	TestTrue(TEXT("its goal holds stand 1"), Traffic->IsStandHeld(Pose(1), 0));
	TestEqual(TEXT("and nothing fires for a stand taken"), Seen.Stands.Num(), 1);
	Traffic->RetireAgent(Plane);
	if (TestEqual(TEXT("the agent gone: stand 1 freed at once"), Seen.Stands.Num(), 2))
	{
		TestTrue(TEXT("stand 1 alone"), Seen.Stands[1].Num() == 1 && Seen.Stands[1][0] == Pose(1));
	}

	// A HELD STAND DELETED, BOTH HELD (review I2): the rebuild reports the deleted one - and ONLY it. Before the
	// rebuild kept its own holds (review I1) every hold read as freed here, so "contains stand 0" measured nothing.
	const FGuidelineNodeId Old = Pose(0);
	const FGuidelineNodeId Kept = Pose(1);
	TestTrue(TEXT("stand 0 held again"), Traffic->HoldStand(-8, Old));
	TestTrue(TEXT("and stand 1"), Traffic->HoldStand(-9, Kept));
	Traffic->Advance(0.05, Net);
	const int32 Before = Seen.Stands.Num();
	Net->RemoveEntity(Field.Stands[0]);
	TestGraph::Rebuild(*Net);
	Traffic->OnGraphRebuilt(*Net);
	if (TestEqual(TEXT("the rebuild reports one broadcast"), Seen.Stands.Num(), Before + 1))
	{
		TestTrue(TEXT("naming the deleted stand 0 alone - stand 1's hold survived the rebuild"),
			Seen.Stands.Last().Num() == 1 && Seen.Stands.Last()[0] == Old);
	}
	TestEqual(TEXT("stand 1 is still held by its flight"), Traffic->HolderOfNode(Kept), -9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedRebuildKeepsHoldsTest, "Airside.Model.Traffic.RebuildKeepsStandHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedRebuildKeepsHoldsTest::RunTest(const FString&)
{
	// REVIEW I1: a guideline rebuild released every Node claim - flight holds with the agents' - and only a load ever
	// re-held them, so any edit in play left accepted flights' stands open to the next accept. The traffic model made
	// these reservations and now keeps them through its own rebuild, re-held on the same stand ENTITY.
	FTestAirportOptions Options;
	Options.StandCount = 2;
	const FTestAirport Field = FTestAirport::Build(TestAirframes::Piper(), Options);
	URoadNetwork* Net = Field.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRecorder Seen;
	Seen.Bind(*Traffic);
	TestTrue(TEXT("stand 0 held for a flight"), Traffic->HoldStand(-7, Field.Pose(Field.Stands[0])));
	TestTrue(TEXT("stand 1 held for another"), Traffic->HoldStand(-8, Field.Pose(Field.Stands[1])));
	Traffic->Advance(0.05, Net);

	TestGraph::Rebuild(*Net);
	Traffic->OnGraphRebuilt(*Net);
	TestEqual(TEXT("after the rebuild stand 0 is still held, by its own holder"), Traffic->HolderOfNode(Field.Pose(Field.Stands[0])), -7);
	TestEqual(TEXT("and stand 1 by its"), Traffic->HolderOfNode(Field.Pose(Field.Stands[1])), -8);
	TestEqual(TEXT("so nothing is reported freed"), Seen.Stands.Num(), 0);

	// A HOLD WHOSE STAND IS GONE is dropped: nothing to re-hold, and the flight's dead Stand is HeldStandLost's evidence.
	Net->RemoveEntity(Field.Stands[0]);
	TestGraph::Rebuild(*Net);
	Traffic->OnGraphRebuilt(*Net);
	TestFalse(TEXT("the deleted stand's holder holds nothing"),
		Traffic->GetOccupancy().GetClaims().ContainsByPredicate([](const FTrafficClaim& C) { return C.AgentId == -7; }));
	TestEqual(TEXT("the other hold still stands"), Traffic->HolderOfNode(Field.Pose(Field.Stands[1])), -8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedRebuildCrossingTest, "Airside.Model.Traffic.RunwayFreed.RebuildKeepsCrossingHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedRebuildCrossingTest::RunTest(const FString&)
{
	// THE REBUILD WINDOW (push-ground-freed review I1), asked of the runway signal: a rebuild drops guideline claims, and
	// the diff it runs must not read a strip a taxiing aircraft is crossing as freed. Surface claims are not guideline
	// claims (ReleaseGuidelineClaims keeps them), so this held before the fix too - pinned, since the arrival queue lands
	// on this event.
	FCrossing C;
	if (!TestTrue(TEXT("dispatched across the strip"), C.Build())) { return false; }
	FRecorder Seen;
	Seen.Bind(*C.Traffic);
	if (!TestTrue(TEXT("the crossing takes the strip"), C.TickUntil(120.0, [&C]() { return C.Held(); }))) { return false; }
	C.Traffic->OnGraphRebuilt(*C.Net);
	TestTrue(TEXT("after the rebuild the strip is still held"), C.Held());
	TestEqual(TEXT("and the rebuild reported no runway freed"), Seen.Runways.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayFreedRebuildLeavingStandTest, "Airside.Model.Traffic.RunwayFreed.RebuildKeepsLeavingStandHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRunwayFreedRebuildLeavingStandTest::RunTest(const FString&)
{
	// THE REBUILD WINDOW, asked of the stand signal: an aircraft departing a stand holds it with its BODY only - its goal
	// is the runway now. The rebuild dropped that body claim until the next Advance, so its diff reported the stand
	// freed with the aircraft still on it, and an arrival queue woken by it could send a flight to that stand.
	FTestAirportOptions Options;
	Options.StandCount = 1;
	const FTestAirport Field = FTestAirport::Build(TestAirframes::Piper(), Options);
	URoadNetwork* Net = Field.Net;
	const FGuidelineNodeId Pose = Net->GetEntity(Field.Stands[0])->PoseNode;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRecorder Seen;
	Seen.Bind(*Traffic);

	const FGuidelineNodeId TaxiEnd = RouteSearch::FindNearestNode(*Net, Field.ExitAt + FVector2D(0.0, -20000.0),
		ETraversalClass::Aircraft, 5000.0);
	const FRoutePlan ToStand = TestGraph::Probe(*Net, TaxiEnd, Pose, ETraversalClass::Aircraft);
	const int32 Plane = ToStand.IsValid() ? Traffic->DispatchAgent(Net, ToStand, TestAirframes::Piper(), ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("an aircraft heads for the stand"), Plane > 0)) { return false; }
	for (int32 Tick = 0; Tick < 20000 && Traffic->FindAgent(Plane)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestEqual(TEXT("parked"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Parked)) { return false; }
	if (!TestEqual(TEXT("and sent to depart"), Traffic->DepartAgent(Plane, *Net), EDepartureRefusal::None)) { return false; }
	Traffic->Advance(1.0 / 30.0, Net);
	if (!TestTrue(TEXT("one frame on, its body still holds the stand"), Traffic->IsStandHeld(Pose, 0))) { return false; }
	const int32 Before = Seen.Stands.Num();

	Traffic->OnGraphRebuilt(*Net);
	TestTrue(TEXT("after the rebuild the stand is still held by the body on it"), Traffic->IsStandHeld(Pose, 0));
	TestEqual(TEXT("and the rebuild reported no stand freed"), Seen.Stands.Num(), Before);
	return true;
}

#endif
