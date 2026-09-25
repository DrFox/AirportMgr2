#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
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
	/** The three content tiers, narrow first, or an empty array when the set does not have them. */
	TArray<URoadProfile*> Tiers()
	{
		TArray<URoadProfile*> Out;
		const UAirsideContent* Content = UAirsideSettings::GetContent();
		if (Content == nullptr || Content->ServiceRoadProfiles.Num() != 3)
		{
			return Out;
		}
		for (const TSoftObjectPtr<URoadProfile>& Tier : Content->ServiceRoadProfiles)
		{
			Out.Add(Tier.LoadSynchronous());
		}
		return Out;
	}

	/** A 30 m stub (0,0)->(3000,0) of Profile, derived as a rebuild derives it; returns the lanes' outer ends. */
	URoadNetwork* DeadEnd(URoadProfile* Profile, const FRoadDesignVehicles& Designs, FGuidelineNodeId& OutIn, FGuidelineNodeId& OutBack)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Seg = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(3000.0, 0.0)), Profile);
		FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net, 12, &Designs), Designs);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignVehicleWideDeadEndTest, "Airside.Build.DesignVehicle.WideDeadEndAdmitsRig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDesignVehicleWideDeadEndTest::RunTest(const FString& Parameters)
{
	using namespace DesignVehicle;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }

	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const TCHAR* const Names[3] = { TEXT("Narrow"), TEXT("Standard"), TEXT("Wide") };

	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		FGuidelineNodeId In, Back;
		const URoadNetwork* Net = DeadEnd(Profiles[Tier], Designs, In, Back);
		if (!TestTrue(FString::Printf(TEXT("%s: the stub has both lanes"), Names[Tier]), In.IsSet() && Back.IsSet())) { continue; }

		const FRoutePlan RigPlan = RoundTheEnd(*Net, In, Back, Rig);
		const FRoutePlan BowserPlan = RoundTheEnd(*Net, In, Back, Bowser);
		// THE BOWSER, UNCHANGED: every tier's dead end still turns it - Narrow and Standard are
		// still laid for it, and Wide's larger balloon only has more room.
		TestTrue(FString::Printf(TEXT("%s: the bowser turns at the dead end, as on every tier before"), Names[Tier]),
			BowserPlan.IsValid());

		if (Tier == UAirsideSettings::WideServiceTier)
		{
			if (!TestTrue(TEXT("Wide: the rig is routed round the dead end - its tier is laid for it"), RigPlan.IsValid())) { continue; }
			// AND DRIVES IT WITHOUT FOLDING: a balloon sized at the rig's bare lock was admitted by
			// the router and folded the trailer on the agent (measured 2026-09-25) - the admission
			// is only worth what the driving agent does with it.
			FRoadAgent Agent;
			Agent.StartDrive(RigPlan, Rig);
			FAgentMotion Motion;
			EAgentEvent Event;
			for (int32 Frame = 0; Frame < 6000 && Agent.GetJackknifedLink() == INDEX_NONE && Agent.Phase != EAgentPhase::Parked; ++Frame)
			{
				Agent.Advance(0.05, Motion, Event);
			}
			TestEqual(TEXT("Wide: the rig's trailer never jack-knifed going round"), Agent.GetJackknifedLink(), static_cast<int32>(INDEX_NONE));
			TestEqual(TEXT("Wide: and it got all the way round"), static_cast<int32>(Agent.Phase), static_cast<int32>(EAgentPhase::Parked));
			continue;
		}
		// NARROW AND STANDARD STILL REFUSE THE RIG, and for the reason the course logged before
		// the ruling: the balloon is tighter than its lock.
		TestFalse(FString::Printf(TEXT("%s: the rig is still refused at the dead end"), Names[Tier]), RigPlan.IsValid());
		const FGuidelineEdge* Rejected = RigPlan.RejectedEdge.IsSet() ? Net->GetGuidelineEdge(RigPlan.RejectedEdge) : nullptr;
		if (!TestNotNull(FString::Printf(TEXT("%s: the refusal names the edge"), Names[Tier]), Rejected)) { continue; }
		const FFitVerdict Verdict = VehicleFit::Judge(*Rejected, Rig, *Net);
		TestEqual(FString::Printf(TEXT("%s: refused as tighter than the rig's lock (%s)"), Names[Tier], *Verdict.Describe()),
			static_cast<int32>(Verdict.Refusal), static_cast<int32>(EFitRefusal::TighterThanLock));
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

#endif

