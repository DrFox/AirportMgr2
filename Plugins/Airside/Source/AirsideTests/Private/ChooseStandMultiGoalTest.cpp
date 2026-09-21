#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A star: From at the origin, with one guideline edge running straight to each stand's
	 * own pose node - so an edge's cached Length (#171) IS the Euclidean distance AddStand
	 * was asked for, with no FAnchorLink lead-in geometry to blur it. URoadNetwork::
	 * PlaceEntity gives every entity a raw, non-derived PoseNode at the placement position
	 * regardless of anchors, so no anchor is needed at all for a route query to reach one.
	 */
	struct FStarFixture
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId From;
		TArray<FGuidelineNodeId> Poses;

		/**
		 * Adds one stand DistanceUu from From along Direction, in FEntityInstance ENUMERATION
		 * ORDER (GetEntities(), the order PlaceEntity is called in) - ArrivalPlanner::
		 * ChooseStand's tie-break depends on that order, which is why every test below calls
		 * this in the exact sequence its comment says to.
		 *
		 * bJoin=false leaves the pose with no edge at all - PlaceEntity's PoseNode carries none
		 * until one is drawn to it, so this is the "never touched by the search" candidate,
		 * built without a second code path.
		 */
		FGuidelineNodeId AddStand(double DistanceUu, const FVector2D& Direction, bool bJoin = true)
		{
			UEntityDefinition* Definition = UEntityDefinition::MakeStandTransient();
			const FVector2D At = Net->GetGuidelineNode(From)->Position + Direction.GetSafeNormal() * DistanceUu;
			const FEntityInstanceId Id = Net->PlaceEntity(Definition, Definition->Anchors, At, 0.0);
			const FGuidelineNodeId Pose = Net->GetEntity(Id)->PoseNode;
			if (bJoin)
			{
				TestGraph::Join(*Net, From, Pose);
			}
			Poses.Add(Pose);
			return Pose;
		}
	};

	FStarFixture BuildStar()
	{
		FStarFixture Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		Out.From = TestGraph::Node(*Out.Net, 0.0, 0.0);
		return Out;
	}

	/** One TryClaim on Node for Agent - StandChoiceTest's own Hold lambda, repeated here
	 *  because it is three lines and pulling in GroundTraffic for it would be a stranger
	 *  dependency than writing them again. */
	void Hold(FTrafficOccupancy& Occ, FGuidelineNodeId Node, int32 Agent)
	{
		FTrafficClaim Claim;
		Claim.AgentId = Agent;
		Claim.Resource = FTrafficResource::OfNode(Node);
		Claim.Rank = 10;
		FTrafficClaim Bumped;
		Occ.TryClaim(Claim, Bumped);
	}
}

// #190, deferred from #171/#201: ArrivalPlanner::ChooseStand used to run one RouteSearch::
// Find per candidate stand; it now runs ONE RouteSearch::FindToGoals whose goal set is every
// candidate at once (see ArrivalPlanner.cpp). The tie-break the old per-stand loop implied -
// first STRICTLY-lower length wins, so a LATER equal length never overtakes it - has to
// survive that change exactly, because the traffic tests (StandChoiceTest and friends) pin
// which stand an arrival takes. Built so the answer is NOT "nearest" and NOT "first placed"
// by accident: index 0 is beaten on length, indices 1 and 2 tie exactly, index 3 is nearer
// still but held, and index 4 is never reachable at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FChooseStandMultiGoalTieBreakTest,
	"Airside.Model.ArrivalPlanner.ChooseStandMultiGoalTieBreak",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FChooseStandMultiGoalTieBreakTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();

	FStarFixture Star = BuildStar();
	const FGuidelineNodeId Far = Star.AddStand(5000.0, FVector2D(1.0, 0.0));                    // index 0
	const FGuidelineNodeId TieA = Star.AddStand(2000.0, FVector2D(0.0, 1.0));                   // index 1 - wins
	const FGuidelineNodeId TieB = Star.AddStand(2000.0, FVector2D(0.0, -1.0));                  // index 2 - loses
	const FGuidelineNodeId HeldNearest = Star.AddStand(1000.0, FVector2D(-1.0, 0.0));           // index 3
	const FGuidelineNodeId Unreachable = Star.AddStand(1.0, FVector2D(1.0, 1.0), /*bJoin=*/false); // index 4

	FTrafficOccupancy Occ;
	Hold(Occ, HeldNearest, /*Agent=*/7);

	FRoutePlan Route;
	bool bSawHeld = false;
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(
		*Star.Net, Star.From, Airframe, &Occ, /*ExcludingAgent=*/0, &Route, &bSawHeld);

	TestEqual(TEXT("the first of the two exactly-tied stands wins, not the nearer held one"), Chosen, TieA);
	if (TestTrue(TEXT("its route is the one the search actually returns"), Route.IsValid()))
	{
		TestTrue(FString::Printf(TEXT("and its length is the tied distance (%.3f), not the held stand's"), Route.Length),
			FMath::IsNearlyEqual(Route.Length, 2000.0, 1.0));
	}
	TestTrue(TEXT("bSawHeld reports the nearer stand it had to skip"), bSawHeld);
	TestTrue(TEXT("neither the farther stand nor the loser of the tie was chosen"), Chosen != Far && Chosen != TieB);
	TestTrue(TEXT("the never-joined stand did not crash the search or win it"), Chosen != Unreachable);

	return true;
}

// The SAME tie, with the two candidates placed in the OPPOSITE order - proving the winner
// follows FEntityInstance ENUMERATION ORDER (GetEntities()), not an artifact of which one a
// TMap/TSet-backed search happens to settle or iterate first (neither gives any ordering
// guarantee at all, which is exactly what swapping the search backend could have broken).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FChooseStandMultiGoalTieBreakOrderTest,
	"Airside.Model.ArrivalPlanner.ChooseStandMultiGoalTieBreakOrderIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FChooseStandMultiGoalTieBreakOrderTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	FStarFixture Star = BuildStar();
	const FGuidelineNodeId TieB = Star.AddStand(2000.0, FVector2D(0.0, -1.0)); // index 0 this time
	const FGuidelineNodeId TieA = Star.AddStand(2000.0, FVector2D(0.0, 1.0));  // index 1 this time

	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, Airframe, nullptr, 0);

	TestEqual(TEXT("swapping placement order swaps the winner - the tie-break is index-driven"), Chosen, TieB);
	TestTrue(TEXT("not the stand that happened to keep the earlier test's name"), Chosen != TieA);
	return true;
}

// THE MEASUREMENT #190 EXISTS FOR: the old per-stand loop ran one full graph search
// (RouteSearch::Find, which itself runs RunSearch) per CANDIDATE STAND; ChooseStand now runs
// exactly one (RouteSearch::FindToGoals) regardless of how many stands there are. Six stands
// and a hold thrown in so the count cannot be hiding a per-stand call behind the held-skip
// branch, which never used to call RouteSearch::Find at all for a stand skipped that way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FChooseStandMultiGoalSearchCountTest,
	"Airside.Model.ArrivalPlanner.ChooseStandSearchCount",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FChooseStandMultiGoalSearchCountTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	FStarFixture Star = BuildStar();
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Star.AddStand(1000.0 + 100.0 * Index, FVector2D(1.0, static_cast<double>(Index)));
	}

	FTrafficOccupancy Occ;
	Hold(Occ, Star.Poses[2], /*Agent=*/7);

	RouteSearch::ResetSearchCallCountForTest();
	ArrivalPlanner::ChooseStand(*Star.Net, Star.From, Airframe, &Occ, 0);

	TestEqual(TEXT("one multi-goal search settles all six stands, not one search each"),
		RouteSearch::SearchCallCountForTest(), 1);
	return true;
}

#endif
