#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/AgentMotion.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// PER-TIER DESIGN VEHICLES (user ruling 2026-09-25; spec 2026-09-24 §3 REVISED): the Wide
// service-road tier's corners and dead ends are laid for the articulated rig, Narrow and
// Standard for the bowser as before. Every network here is derived the way a rebuild derives
// it - UAirsideSettings::ResolveRoadDesignVehicles, handed to the solver and the builder - so
// what is measured is the production path, not a uniform-vehicle fixture.

namespace DesignVehicle
{
	/** The three content tiers, narrow first, or an empty array when the set does not have them.
	 *  #310: was its own copy, byte-identical to BendLaneTest's and WidthTaperTest's. */
	TArray<URoadProfile*> Tiers() { return TestProfiles::ServiceTiers(); }

	/** A 30 m stub (0,0)->(3000,0) of Profile, derived as a rebuild derives it; returns the lanes' outer ends. */
	URoadNetwork* DeadEnd(URoadProfile* Profile, const FRoadDesignVehicles& Designs, FGuidelineNodeId& OutIn, FGuidelineNodeId& OutBack)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Seg = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(3000.0, 0.0)), Profile);
		TestGraph::Derive(*Net, &Designs);
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != Seg) { continue; }
			if (Edge.Direction == EGuidelineDir::AToB) { OutIn = Edge.A; }
			if (Edge.Direction == EGuidelineDir::BToA) { OutBack = Edge.B; }
		}
		return Net;
	}

	/** Down the stub, round its balloon and back, with Vehicle's body. */
	FRoutePlan RoundTheEnd(const URoadNetwork& Net, FGuidelineNodeId In, FGuidelineNodeId Back, const FVehicle& Vehicle)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, In, Back, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Vehicle);
		return RouteSearch::Find(Net, Query);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignVehicleDeadEndTest, "Airside.Build.DesignVehicle.WideDeadEndRefusesRigUntilReversing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDesignVehicleDeadEndTest::RunTest(const FString& Parameters)
{
	// BALLOONS ARE NOT PER TIER, BY RULING (2026-09-25, "smaller, reverse later"): every tier's
	// dead end is sized for the bowser, the Wide one included. RESHAPED THE SAME DAY within that
	// footprint (Airside.Solve.UTurnBalloonFootprint): every piece now clears the rig's LOCK, and
	// the whole-route tow check (VehicleFit::JudgePlan) is what refuses it - its trailer folds
	// going round, on all three tiers. So the rig is still refused at every dead end until
	// reversing (step 2) gives it a three-point turn; the reason moved from the lock to the fold.
	// When reversing lands, the lines here are the ones to flip.
	using namespace DesignVehicle;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }

	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const FVehicle Utility = UAirsideSettings::ResolveUtilityTowVehicle();
	const TCHAR* const Names[3] = { TEXT("Narrow"), TEXT("Standard"), TEXT("Wide") };

	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		FGuidelineNodeId In, Back;
		const URoadNetwork* Net = DeadEnd(Profiles[Tier], Designs, In, Back);
		if (!TestTrue(FString::Printf(TEXT("%s: the stub has both lanes"), Names[Tier]), In.IsSet() && Back.IsSet())) { continue; }

		// THE BOWSER, UNCHANGED: every tier's dead end turns it, as before any of this.
		TestTrue(FString::Printf(TEXT("%s: the bowser turns at the dead end"), Names[Tier]),
			RoundTheEnd(*Net, In, Back, Bowser).IsValid());
		// AND THE UTILITY'S DRAWBAR TRAIN, now judged over the whole route like any tow: it holds.
		const FRoutePlan UtilityPlan = RoundTheEnd(*Net, In, Back, Utility);
		TestTrue(FString::Printf(TEXT("%s: the utility and its trailer turn at the dead end (%s)"), Names[Tier],
			*UtilityPlan.RejectedBy.Describe()), UtilityPlan.IsValid());

		// THE RIG: every piece fits it on its own now - the lock is no longer why...
		FRouteQuery Bare = FRouteQuery::For(ERouteErrand::GraphProbe, In, Back, 0.0, ETraversalClass::GroundVehicle);
		const FRoutePlan Unjudged = RouteSearch::Find(*Net, Bare);
		if (!TestTrue(FString::Printf(TEXT("%s: the lanes and the balloon join up"), Names[Tier]), Unjudged.IsValid())) { continue; }
		double Tightest = TNumericLimits<double>::Max();
		bool bEveryEdgeFits = true;
		for (const FRouteStep& Step : Unjudged.Steps)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
			bEveryEdgeFits &= Edge != nullptr && VehicleFit::Fits(*Edge, Rig, *Net);
			if (Edge != nullptr && Edge->MinRadius > 0.0) { Tightest = FMath::Min(Tightest, Edge->MinRadius); }
		}
		TestTrue(FString::Printf(TEXT("%s: every edge round the end fits the rig on its own - tightest %.0f uu vs its %.0f lock"), Names[Tier],
			Tightest, Rig.Chassis.TightestFollowableRadius()), bEveryEdgeFits);

		// ...and the whole route refuses it: the trailer folds.
		const FRoutePlan RigPlan = RoundTheEnd(*Net, In, Back, Rig);
		AddInfo(FString::Printf(TEXT("%s: rig %s"), Names[Tier], *RigPlan.RejectedBy.Describe()));
		TestFalse(FString::Printf(TEXT("%s: the rig is refused at the dead end until reversing exists"), Names[Tier]), RigPlan.IsValid());
		TestEqual(FString::Printf(TEXT("%s: as a road it does not fit - TooNarrow"), Names[Tier]),
			static_cast<int32>(RigPlan.Result), static_cast<int32>(ERouteResult::TooNarrow));
		TestTrue(FString::Printf(TEXT("%s: on the whole route"), Names[Tier]), RigPlan.RejectedBy.bWholeRoute);
		TestEqual(FString::Printf(TEXT("%s: because its trailer folds going round (%s)"), Names[Tier], *RigPlan.RejectedBy.Describe()),
			static_cast<int32>(RigPlan.RejectedBy.Refusal), static_cast<int32>(EFitRefusal::TrailerFolds));

		// THE REFUSAL IS THE AGENT'S (one evaluator, on the real balloon): dispatched round it
		// anyway, the rig jack-knifes.
		FRoadAgent Agent;
		Agent.StartDrive(Unjudged, Rig);
		FAgentMotion Motion;
		EAgentEvent Event = EAgentEvent::None;
		for (int32 Frame = 0; Frame < 30 * 600 && Event != EAgentEvent::Parked && Agent.GetJackknifedLink() == INDEX_NONE; ++Frame)
		{
			Agent.Advance(1.0 / 30.0, Motion, Event);
		}
		TestEqual(FString::Printf(TEXT("%s: driven round it anyway, the rig's trailer jack-knifes"), Names[Tier]),
			Agent.GetJackknifedLink(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignVehicleFilletTest, "Airside.Build.DesignVehicle.FilletPerTier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDesignVehicleFilletTest::RunTest(const FString& Parameters)
{
	using namespace DesignVehicle;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }

	const FChassis Bowser = UAirsideSettings::ResolveLargestServiceVehicle();
	const double RigLock = UAirsideSettings::ResolveRigVehicle().Chassis.TightestFollowableRadius();
	TestTrue(TEXT("the rig's lock is wider than the bowser's - otherwise this ruling changes nothing"),
		RigLock > Bowser.TightestFollowableRadius());

	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const URoadProfile* Profile = Profiles[Tier];
		// THE SELF-RESOLVING AND THE PASSED-DOWN PATH AGREE: a rebuild and a lone caller must lay
		// the same corner, or the #190 split would be two answers.
		TestEqual(FString::Printf(TEXT("tier %d: the profile's own fillet equals the one a rebuild passes down"), Tier),
			Profile->ResolvedFilletRadius(), Profile->ResolvedFilletRadius(Designs.For(Profile)));
		if (Tier == UAirsideSettings::WideServiceTier)
		{
			TestTrue(FString::Printf(TEXT("Wide: the corner fillet (%.0f uu) is at least the rig's lock radius (%.0f uu)"),
				Profile->ResolvedFilletRadius(), RigLock), Profile->ResolvedFilletRadius() >= RigLock);
			// AND IS THE RIG'S, NOT THE BOWSER'S WITH MARGIN: the bowser's lock times the junction
			// margin already clears 576 uu, so the bound above alone would pass with no ruling at
			// all (measured 2026-09-25). The fillet must be the one derived from the rig.
			TestEqual(TEXT("Wide: the corner fillet is derived from the rig, its design vehicle"),
				Profile->ResolvedFilletRadius(), Profile->ResolvedFilletRadius(UAirsideSettings::ResolveRigVehicle().Chassis));
			TestTrue(TEXT("Wide: which is wider than the bowser would have been given"),
				Profile->ResolvedFilletRadius() > Profile->ResolvedFilletRadius(Bowser));
		}
		else
		{
			TestEqual(FString::Printf(TEXT("tier %d: the corner fillet is the bowser's, unchanged"), Tier),
				Profile->ResolvedFilletRadius(), Profile->ResolvedFilletRadius(Bowser));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignVehicleTierCacheTest, "Airside.Build.DesignVehicle.TierResolvedOncePerContent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDesignVehicleTierCacheTest::RunTest(const FString& Parameters)
{
	// THE SNAP AND GHOST PATHS, in the style of LargestServiceVehicleResolvedOncePerRebuild
	// (issue #190): URoadProfile's self-resolving fillet asks the tier map per arm, and a cursor
	// move asks it per arm of every node it tests. Resolving the content set, loading the Wide
	// profile and building the rig each time is the #190/#167 cost; it must resolve ONCE.
	using namespace DesignVehicle;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadSegmentId West = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-20000.0, 0.0)), Profiles[2]);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(20000.0, 0.0)), Profiles[2]);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 20000.0)), Profiles[0]);

	UAirsideSettings::ResetTierDesignVehiclesCacheForTest();
	// SELF-RESOLVING, as RoadSnap and the debug gallery call it: no vehicles passed.
	FRoadNetworkSolver::SolveAll(*Net);
	for (int32 Move = 0; Move < 50; ++Move)
	{
		FRoadNetworkSolver::NodeClaims(*Net, Hub, FVector2D(Move * 10.0, 100.0));
		FRoadNetworkSolver::ArmCutDistance(*Net, West, Hub);
	}
	TestEqual(TEXT("a solve and fifty cursor moves over a Wide junction resolve the tier map once"),
		UAirsideSettings::ResolveTierDesignVehiclesCallCountForTest, 1);

	// And the cached answer is the resolved one: the Wide arm's fillet is still the rig's.
	TestEqual(TEXT("the cached tier map still lays the Wide fillet for the rig"),
		Profiles[2]->ResolvedFilletRadius(), Profiles[2]->ResolvedFilletRadius(UAirsideSettings::ResolveRigVehicle().Chassis));
	TestEqual(TEXT("and asking again did not resolve again"), UAirsideSettings::ResolveTierDesignVehiclesCallCountForTest, 1);

	// A COLD CACHE RESOLVES AGAIN - the counter counts misses, not calls.
	UAirsideSettings::ResetTierDesignVehiclesCacheForTest();
	Profiles[2]->ResolvedFilletRadius();
	TestEqual(TEXT("after a reset, one resolve"), UAirsideSettings::ResolveTierDesignVehiclesCallCountForTest, 1);
	return true;
}

#endif

