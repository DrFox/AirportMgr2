#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * UGroundTraffic::OnPushGroundFreed - the push watch (ops follow-up to batch 3 PR D). A parked aircraft DepartAgent refused
 * PushbackBlocked is re-asked DepartAgent's own question at every DiffFreedom, and the delegate fires once when the answer
 * stops being PushbackBlocked. Each test fails if the watch, its diff, or its re-derive on a rebuild is unwired.
 */
namespace PushGroundFreedTest
{
	constexpr double Frame = 1.0 / 30.0;

	/**
	 * PushbackDepartTest's field (a runway, stand A south of junction J, the far arm E a push reverses onto) with the far arm
	 * carried on east to F, so a second aircraft can drive off the push ground and out of the way. Taxi B -> A and the
	 * aeroplane parks facing south with its way out behind it: a push onto J-E. Prefixed: unity build.
	 *
	 *   B (0, 0) on runway 1 . J (0, -10000) . A (0, -20000) the stand . E (20000, -10000) . F (100000, -10000)
	 *
	 * bTwoRunways adds runway 2, north-south through F: a departure there leaves J by E, so its push reverses onto J-B.
	 */
	struct FPushField
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		FGuidelineNodeId A, B, J, E, F;
		FGuidelineEdgeId AJ, JE, JB;
		FRoadSegmentId Runway1;
		FRoadSegmentId Runway2;

		void Build(bool bTwoRunways)
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			URoadProfile* Strip = TestProfiles::Runway();
			const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
			const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
			const FRoadNodeId RB = Net->AddNode(FVector2D(bTwoRunways ? 60000.0 : 50000.0, 0.0));
			Runway1 = Net->AddStraightSegment(RA, RM, Strip);
			Net->AddStraightSegment(RM, RB, Strip);
			if (bTwoRunways)
			{
				// CLEAR OF RUNWAY 1'S EAST END, so the two strips never cross.
				const FRoadNodeId SA = Net->AddNode(FVector2D(100000.0, -90000.0));
				const FRoadNodeId SM = Net->AddNode(FVector2D(100000.0, -10000.0));
				const FRoadNodeId SB = Net->AddNode(FVector2D(100000.0, 70000.0));
				Runway2 = Net->AddStraightSegment(SA, SM, Strip);
				Net->AddStraightSegment(SM, SB, Strip);
			}

			B = TestGraph::Node(*Net, 0.0, 0.0);
			J = TestGraph::Node(*Net, 0.0, -10000.0);
			A = TestGraph::Node(*Net, 0.0, -20000.0);
			E = TestGraph::Node(*Net, 20000.0, -10000.0);
			F = TestGraph::Node(*Net, 100000.0, -10000.0);
			// AUTHORED, as PushbackDepartTest's are: they survive OnGraphRebuilt's sweep.
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			AJ = TestGraph::Join(*Net, A, J, Options);
			JB = TestGraph::Join(*Net, J, B, Options);
			JE = TestGraph::Join(*Net, J, E, Options);
			TestGraph::Join(*Net, E, F, Options);
		}

		/** Taxis an aircraft From -> To; its id once Parked, else 0. */
		int32 Park(FGuidelineNodeId From, FGuidelineNodeId To)
		{
			const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, From, To, ETraversalClass::Aircraft),
				TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
			for (int32 I = 0; Id > 0 && I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
			{
				Traffic->Advance(Frame, Net);
			}
			return Id > 0 && Traffic->FindAgent(Id)->Phase == EAgentPhase::Parked ? Id : 0;
		}

		/** A second aircraft set off from E to F - standing on the push ground's far node until it drives off. */
		int32 SendBlocker()
		{
			const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, E, F, ETraversalClass::Aircraft),
				TestAirframes::Piper(), ETraversalClass::Aircraft, 0.0);
			Traffic->Advance(Frame, Net);
			return Id;
		}

		/** The push ground as the test sees it (the lead-in, J, J-E, E), held by anyone but Parked. The oracle the
		 *  delegate's timing is checked against - not the code under test's own route. */
		bool IsPushGroundHeld(int32 Parked) const
		{
			const TArray<FTrafficResource> Ground{ FTrafficResource::OfEdge(AJ), FTrafficResource::OfNode(J),
				FTrafficResource::OfEdge(JE), FTrafficResource::OfNode(E) };
			return Traffic->GetOccupancy().IsAnyHeld(Ground, Parked, /*bCountOwnOccupied*/ false);
		}
	};
}

using PushGroundFreedTest::FPushField;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedTaxiingTest, "Airside.Model.Traffic.PushGroundFreed.TaxiingBlockerClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedTaxiingTest::RunTest(const FString&)
{
	FPushField Field;
	Field.Build(/*bTwoRunways*/ false);
	const int32 Parked = Field.Park(Field.B, Field.A);
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Parked > 0)) { return false; }
	const int32 Blocker = Field.SendBlocker();
	if (!TestTrue(TEXT("a second aircraft stands on the push ground"), Blocker > 0 && Field.IsPushGroundHeld(Parked))) { return false; }

	TArray<int32> Fired;
	int32 Frame = 0;
	int32 FiredAt = INDEX_NONE;
	Field.Traffic->OnPushGroundFreed.AddLambda([&Fired, &Frame, &FiredAt](int32 Id) { Fired.Add(Id); FiredAt = Frame; });

	TestEqual(TEXT("its push is refused for the aircraft on it"), Field.Traffic->DepartAgent(Parked, *Field.Net),
		EDepartureRefusal::PushbackBlocked);
	TestEqual(TEXT("and it is watched"), Field.Traffic->PushWatchCountForTest(), 1);

	// THE CLAIM PASS FREES IT, with no event of its own: the blocker simply drives on. Nobody re-asks DepartAgent here.
	int32 HeldFrames = 0;
	bool bFiredWhileHeld = false;
	for (Frame = 1; Frame <= 3000; ++Frame)
	{
		Field.Traffic->Advance(PushGroundFreedTest::Frame, Field.Net);
		const bool bHeld = Field.IsPushGroundHeld(Parked);
		HeldFrames += bHeld ? 1 : 0;
		bFiredWhileHeld |= bHeld && FiredAt == Frame;
		if (FiredAt != INDEX_NONE && Frame > FiredAt + 300)
		{
			break;
		}
	}
	TestTrue(FString::Printf(TEXT("the blocker was on the ground for a while first (%d frames)"), HeldFrames), HeldFrames > 5);
	TestFalse(TEXT("nothing fired while the ground was still held"), bFiredWhileHeld);
	if (!TestEqual(TEXT("it fired once, when the ground cleared"), Fired.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the parked aircraft"), Fired[0], Parked);
	TestEqual(TEXT("on the first frame the ground was free"), FiredAt, HeldFrames + 1);
	TestEqual(TEXT("and it left the watch"), Field.Traffic->PushWatchCountForTest(), 0);
	// THE ANSWER THE EVENT PROMISED: asked again, DepartAgent starts the push.
	TestEqual(TEXT("asked again, the push is granted"), Field.Traffic->DepartAgent(Parked, *Field.Net), EDepartureRefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedRetiredTest, "Airside.Model.Traffic.PushGroundFreed.RetiredWatchedFiresNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedRetiredTest::RunTest(const FString&)
{
	FPushField Field;
	Field.Build(false);
	const int32 Parked = Field.Park(Field.B, Field.A);
	if (!TestTrue(TEXT("parked"), Parked > 0)) { return false; }
	Field.SendBlocker();
	int32 Fired = 0;
	Field.Traffic->OnPushGroundFreed.AddLambda([&Fired](int32) { ++Fired; });
	if (!TestEqual(TEXT("refused, blocked"), Field.Traffic->DepartAgent(Parked, *Field.Net), EDepartureRefusal::PushbackBlocked)) { return false; }

	// GONE BEFORE ITS GROUND FREED: there is nothing to wake, and a wake naming a missing agent is a lookup for nothing.
	Field.Traffic->RetireAgent(Parked);
	TestEqual(TEXT("the retire drops it from the watch at once (DiffNow)"), Field.Traffic->PushWatchCountForTest(), 0);
	for (int32 Tick = 0; Tick < 1500; ++Tick)
	{
		Field.Traffic->Advance(PushGroundFreedTest::Frame, Field.Net);
	}
	TestFalse(TEXT("the ground did clear"), Field.IsPushGroundHeld(Parked));
	TestEqual(TEXT("and nothing fired for the retired aircraft"), Fired, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedOutsideAdvanceTest, "Airside.Model.Traffic.PushGroundFreed.ReleaseOutsideAdvance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedOutsideAdvanceTest::RunTest(const FString&)
{
	FPushField Field;
	Field.Build(false);
	const int32 Parked = Field.Park(Field.B, Field.A);
	if (!TestTrue(TEXT("parked"), Parked > 0)) { return false; }

	// A HOLD ON THE FAR NODE - AirportOps' kind of claim, made and given back between ticks.
	if (!TestTrue(TEXT("a hold on the push ground"), Field.Traffic->HoldStand(-7, Field.E))) { return false; }
	int32 Fired = 0;
	Field.Traffic->OnPushGroundFreed.AddLambda([&Fired](int32) { ++Fired; });
	if (!TestEqual(TEXT("refused, blocked"), Field.Traffic->DepartAgent(Parked, *Field.Net), EDepartureRefusal::PushbackBlocked)) { return false; }

	// PAUSED: no Advance comes. The release itself says so, as it does for a runway (PR D review M1).
	Field.Traffic->ReleaseHold(-7);
	TestEqual(TEXT("the release fires at once, with no Advance"), Fired, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedRunwayFlipTest, "Airside.Model.Traffic.PushGroundFreed.RunwayFlipReplans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedRunwayFlipTest::RunTest(const FString&)
{
	// THE PUSH ARM FOLLOWS THE RUNWAY: PlanAny ranks a free strip first, and the push reverses onto the arm the departure
	// does NOT take. Runway 1 (north, via B) is nearer, so the push goes onto J-E - held. Runway 1 then gets busy; the
	// departure would now go to runway 2 via E, and push onto J-B, which is free. A watch that re-asked only its stored
	// route would still see J-E held and say nothing.
	FPushField Field;
	Field.Build(/*bTwoRunways*/ true);
	const int32 Parked = Field.Park(Field.B, Field.A);
	if (!TestTrue(TEXT("parked"), Parked > 0)) { return false; }
	if (!TestTrue(TEXT("a hold on E"), Field.Traffic->HoldStand(-7, Field.E))) { return false; }
	int32 Fired = 0;
	Field.Traffic->OnPushGroundFreed.AddLambda([&Fired](int32) { ++Fired; });
	if (!TestEqual(TEXT("refused, blocked on J-E"), Field.Traffic->DepartAgent(Parked, *Field.Net), EDepartureRefusal::PushbackBlocked)) { return false; }
	Field.Traffic->Advance(PushGroundFreedTest::Frame, Field.Net);
	TestEqual(TEXT("still blocked: nothing"), Fired, 0);

	// RUNWAY 1 HELD, straight into the table - the strip, as a landing holds it.
	for (const FTrafficResource& Surface : Field.Net->RunwaySurfaces(Field.Runway1))
	{
		Field.Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(-9, Surface, /*bOccupied*/ true, 2));
	}
	Field.Traffic->Advance(PushGroundFreedTest::Frame, Field.Net);
	TestEqual(TEXT("runway 1 busy: the departure moves to runway 2 and its push to the free arm - fired"), Fired, 1);
	TestNotEqual(TEXT("and DepartAgent agrees: not blocked"), Field.Traffic->DepartAgent(Parked, *Field.Net),
		EDepartureRefusal::PushbackBlocked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedRebuildTest, "Airside.Model.Traffic.PushGroundFreed.RebuildRederives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedRebuildTest::RunTest(const FString&)
{
	// A BLOCKER THE REBUILD KEEPS: an aircraft PARKED on E. A rebuild drops every guideline claim and re-makes only the
	// agents' goals (GroundTrafficRebuild.cpp, ReleaseGuidelineClaims then ClaimGoalNode) - so a taxiing body or a
	// hold on E would read free for the rebuild's own diff, and a parked goal does not.
	FPushField Field;
	Field.Build(false);
	const int32 Parked = Field.Park(Field.B, Field.A);
	if (!TestTrue(TEXT("parked"), Parked > 0)) { return false; }
	const int32 Blocker = Field.Park(Field.F, Field.E);
	if (!TestTrue(TEXT("a second aircraft parked on E"), Blocker > 0)) { return false; }
	int32 Fired = 0;
	Field.Traffic->OnPushGroundFreed.AddLambda([&Fired](int32) { ++Fired; });
	if (!TestEqual(TEXT("refused, blocked"), Field.Traffic->DepartAgent(Parked, *Field.Net), EDepartureRefusal::PushbackBlocked)) { return false; }

	// A REBUILD WHILE BLOCKED: re-derived, still blocked - so still watched, and nothing fired.
	Field.Traffic->OnGraphRebuilt(*Field.Net);
	TestEqual(TEXT("a rebuild with the ground still held fires nothing"), Fired, 0);
	TestEqual(TEXT("and keeps the aircraft watched - re-derived, not cleared"), Field.Traffic->PushWatchCountForTest(), 1);

	// AND THE WATCH IT KEPT STILL WORKS: the blocker goes, the push is free.
	Field.Traffic->RetireAgent(Blocker);
	TestEqual(TEXT("then the ground frees: one fire"), Fired, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
