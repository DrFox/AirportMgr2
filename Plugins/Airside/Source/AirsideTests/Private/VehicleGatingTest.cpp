#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/PlanReResolver.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spec 2026-09-23 §6, test 6: a vehicle is routed only where it fits. NOT by width alone - a
// 2.54 m rig fits a 3 m straight lane, as real lorries do - but through the corners, where its
// trailer's swept path is what the pavement has to hold.

namespace VehicleGating
{
	void Derive(URoadNetwork& Net)
	{
		// #311: this WAS its own SolveAll+Build pair, the exact shape TestGraph::Derive
		// now holds once; kept as a local wrapper for VehicleGating::Derive's one qualified caller.
		TestGraph::Derive(Net);
	}

	/** Straight road (0,0)->(30000,0); returns its A->B lane's ends. */
	URoadNetwork* Straight(URoadProfile* Profile, FGuidelineNodeId& OutFrom, FGuidelineNodeId& OutTo)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Seg = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(30000.0, 0.0)), Profile);
		Derive(*Net);
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == Seg && Edge.Direction == EGuidelineDir::AToB)
			{
				OutFrom = Edge.A;
				OutTo = Edge.B;
			}
		}
		return Net;
	}

	/** A T at the origin, arms west, east and north; from the west arm's far end onto the north arm. */
	URoadNetwork* TurnNorth(URoadProfile* Profile, FGuidelineNodeId& OutFrom, FGuidelineNodeId& OutTo)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadSegmentId West = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-30000.0, 0.0)), Profile);
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(30000.0, 0.0)), Profile);
		const FRoadSegmentId North = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 30000.0)), Profile);
		Derive(*Net);
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive) { continue; }
			// Arms were added FROM the hub: B->A arrives, A->B leaves.
			if (Edge.DerivedFrom == West && Edge.Direction == EGuidelineDir::BToA) { OutFrom = Edge.B; }
			if (Edge.DerivedFrom == North && Edge.Direction == EGuidelineDir::AToB) { OutTo = Edge.B; }
		}
		return Net;
	}

	// #312: already went through FRouteQuery::For, unlike the five other hand-built helpers
	// the issue named - folded onto TestGraph::Probe anyway so the SAME wrapper answers every
	// GraphProbe in the module, rather than two call sites of one pattern.
	FRoutePlan Route(const URoadNetwork& Net, FGuidelineNodeId From, FGuidelineNodeId To, const FVehicle& Vehicle)
	{
		return TestGraph::Probe(Net, From, To, ETraversalClass::GroundVehicle, &Vehicle);
	}
}

using namespace VehicleGating;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleGatingTest, "Airside.Model.VehicleGating",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleGatingTest::RunTest(const FString& Parameters)
{
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	FGuidelineNodeId From, To;

	// STRAIGHT: width only.
	{
		URoadNetwork* Net = Straight(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		TestTrue(TEXT("the rig drives a straight Narrow road - 2.54 m in a 3 m lane, as lorries do"),
			Route(*Net, From, To, Rig).IsValid());
	}
	{
		URoadNetwork* Net = Straight(URoadProfile::MakeServiceRoadTransient(250.0), From, To);
		const FRoutePlan Plan = Route(*Net, From, To, Bowser);
		TestEqual(TEXT("a 2.5 m lane is too narrow for a 2.37 m bowser with its margins"),
			static_cast<int32>(Plan.Result), static_cast<int32>(ERouteResult::TooNarrow));
		TestTrue(TEXT("and the refusal names the edge that refused it"), Plan.RejectedEdge.IsSet());
	}

	// THE CORNER: the swept path.
	{
		URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		const FRoutePlan BowserPlan = Route(*Net, From, To, Bowser);
		if (TestTrue(TEXT("the bowser turns at a Narrow junction"), BowserPlan.IsValid()))
		{
			FSpeedProfile Profile;
			Profile.Build(BowserPlan.Polyline, Bowser.Chassis);
			// The drivability AUTHORITY over the whole admitted route, not a per-edge restatement
			// of its rule (memory: FSpeedProfile is the drivability authority).
			TestFalse(TEXT("and every metre of the route it was given is inside its lock"), Profile.WasTighterThanLock());
		}
		// SINCE 2026-09-24 THE RIG MAKES THIS TURN: simulated rather than steady-state, and
		// swinging across both lanes as a real driver does (VehicleFit). It was refused here
		// under the steady-state model, which the EU turning circle showed to be far too harsh.
		TestTrue(TEXT("the rig gets round a Narrow corner, swinging across both lanes"),
			Route(*Net, From, To, Rig).IsValid());
	}
	{
		// A CORNER TIGHTER THAN THE CAB CAN STEER: a 2 m fillet on a Narrow T.
		URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(300.0, 60.0, 200.0), From, To);
		const FRoutePlan RigPlan = Route(*Net, From, To, Rig);
		TestEqual(TEXT("the rig cannot steer round a 2 m corner"),
			static_cast<int32>(RigPlan.Result), static_cast<int32>(ERouteResult::TooNarrow));
		TestTrue(TEXT("and it says which edge"), RigPlan.RejectedEdge.IsSet());
		const FGuidelineEdge* Rejected = Net->GetGuidelineEdge(RigPlan.RejectedEdge);
		TestTrue(TEXT("which is the corner, not a lane"), Rejected != nullptr && !Rejected->DerivedFrom.IsSet());
	}

	// UNMEASURED EDGES GATE NOTHING (review focus 1): hand-made and saved edges keep routing.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = FVector2D(500.0, 200.0);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Net->AddGuidelineEdge(MoveTemp(Edge));
		TestTrue(TEXT("an edge nobody measured admits even the rig"), Route(*Net, A, B, Rig).IsValid());
	}

	// NO VEHICLE, NO GATE: the query every caller made before this stays exactly as it was.
	{
		URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, From, To, 0.0, ETraversalClass::GroundVehicle);
		TestTrue(TEXT("a query with no vehicle is not gated"), RouteSearch::Find(*Net, Query).IsValid());
	}
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

// Spec §6 / plan Task 5: the Wide tier's corners are sized for the rig, and it is the AUTHORED
// ASSET that is tested - loaded from the content set - so the figure lives in one place
// (build_road_profiles.py writes it) and this test holds it to its job.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigTurnsOnWideTest, "Airside.Model.RigTurnsOnWide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigTurnsOnWideTest::RunTest(const FString& Parameters)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestTrue(TEXT("the content set has three road tiers"), Content != nullptr && Content->ServiceRoadProfiles.Num() == 3))
	{
		return false;
	}
	URoadProfile* Wide = Content->ServiceRoadProfiles[2].LoadSynchronous();
	URoadProfile* Narrow = Content->ServiceRoadProfiles[0].LoadSynchronous();
	if (!TestTrue(TEXT("the Wide and Narrow tiers load"), Wide != nullptr && Narrow != nullptr)) { return false; }

	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();

	// Both turns at a T: the near-side one (W->N, a right turn on screen) is the tight one.
	auto BothWays = [&](URoadProfile* Profile, const FVehicle& Vehicle)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadSegmentId West = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-40000.0, 0.0)), Profile);
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(40000.0, 0.0)), Profile);
		const FRoadSegmentId North = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 40000.0)), Profile);
		VehicleGating::Derive(*Net);
		FGuidelineNodeId WIn, WOut, NIn, NOut;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive) { continue; }
			if (Edge.DerivedFrom == West && Edge.Direction == EGuidelineDir::BToA) { WIn = Edge.B; }
			if (Edge.DerivedFrom == West && Edge.Direction == EGuidelineDir::AToB) { WOut = Edge.B; }
			if (Edge.DerivedFrom == North && Edge.Direction == EGuidelineDir::AToB) { NOut = Edge.B; }
			if (Edge.DerivedFrom == North && Edge.Direction == EGuidelineDir::BToA) { NIn = Edge.B; }
		}
		return VehicleGating::Route(*Net, WIn, NOut, Vehicle).IsValid()
			&& VehicleGating::Route(*Net, NIn, WOut, Vehicle).IsValid();
	};
	TestTrue(TEXT("the rig turns both ways at a Wide junction - the tier it is the design vehicle of"), BothWays(Wide, Rig));
	TestTrue(TEXT("and at a Narrow one too, across both lanes - measured 2026-09-24 once the turn was simulated"),
		BothWays(Narrow, Rig));
	TestTrue(TEXT("the bowser turns both ways at a Narrow junction, as it did before gating"), BothWays(Narrow, Bowser));
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

// Review of 2026-09-24 item 4, and the per-sample rule that replaced the steady-state one.
// The edge is a REAL measured turn from a Wide T (so the path, the samples and the lock are
// real); only its clearances are overridden, so nothing but the clearance comparison decides.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleFitClearanceTest, "Airside.Model.VehicleFitClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleFitClearanceTest::RunTest(const FString& Parameters)
{
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	FGuidelineNodeId From, To;
	URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(450.0, 60.0, 1500.0), From, To);

	const FGuidelineEdge* Found = nullptr;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (Edge.bAlive && !Edge.DerivedFrom.IsSet() && Edge.MinRadius > Rig.Chassis.TightestFollowableRadius()
			&& Edge.ClearInnerAt.Num() > 0 && Edge.Control.Size() < 10000.0)
		{
			Found = &Edge;
			break;
		}
	}
	if (!TestNotNull(TEXT("a measured turn the rig's lock can take"), Found)) { return false; }
	FGuidelineEdge Turn = *Found;

	auto WithClearance = [&Turn](float In, float Out)
	{
		FGuidelineEdge Copy = Turn;
		for (float& V : Copy.ClearInnerAt) { V = In; }
		for (float& V : Copy.ClearOuterAt) { V = Out; }
		return Copy;
	};
	TestTrue(TEXT("with 20 m of tarmac either side the rig fits"),
		VehicleFit::Fits(WithClearance(2000.f, 2000.f), Rig, *Net));
	TestFalse(TEXT("with 1 m either side it does not - the clearance alone refuses"),
		VehicleFit::Fits(WithClearance(100.f, 100.f), Rig, *Net));
	TestTrue(TEXT("a tight inside is fine when the outside has room: the rig swings wide"),
		VehicleFit::Fits(WithClearance(150.f, 2000.f), Rig, *Net));
	FGuidelineEdge Unmeasured = Turn;
	Unmeasured.ClearInnerAt.Reset();
	Unmeasured.ClearOuterAt.Reset();
	TestTrue(TEXT("unmeasured clearance gates nothing - a balloon over grass"), VehicleFit::Fits(Unmeasured, Rig, *Net));

	// JUDGE IS THE RULE WITH ITS REASON (the test course prints it): it must agree with Fits
	// case for case, and a clearance refusal must carry the figures it was refused on.
	const FFitVerdict Refused = VehicleFit::Judge(WithClearance(100.f, 100.f), Rig, *Net);
	TestEqual(TEXT("Judge names the clearance rule for a 2 m road"),
		static_cast<int32>(Refused.Refusal), static_cast<int32>(EFitRefusal::SweptOverTarmac));
	TestEqual(TEXT("and the tarmac it compared against is the 2 m it was given"), Refused.Available, 200.0, 0.01);
	TestTrue(TEXT("and a swept width wider than that"), Refused.Needed > Refused.Available);
	TestFalse(TEXT("and it describes itself"), Refused.Describe().IsEmpty());
	TestTrue(TEXT("Judge agrees with Fits on the fitting case"),
		VehicleFit::Judge(WithClearance(2000.f, 2000.f), Rig, *Net).Fits());
	TestTrue(TEXT("and on the unmeasured one"), VehicleFit::Judge(Unmeasured, Rig, *Net).Fits());
	return true;
}

// Review item 1: every query an AGENT's re-route builds carries its body, so a road edit or a
// deadlock replan cannot send a truck down road it does not fit. One seam, three callers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentQueryCarriesBodyTest, "Airside.Model.AgentQueryCarriesBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentQueryCarriesBodyTest::RunTest(const FString& Parameters)
{
	FRoutePlan Plan;
	FRoadAgent Truck;
	Truck.StartDrive(Plan, UAirsideSettings::ResolveDefaultVehicle());
	const FRouteQuery TruckQuery = FPlanReResolver::QueryFor(ERouteErrand::Replan,
		FGuidelineNodeId(), FGuidelineNodeId(), Truck);
	TestTrue(TEXT("a truck's re-route is gated on the truck"), TruckQuery.Vehicle == Truck.AsVehicle() && TruckQuery.Vehicle != nullptr);

	FRoadAgent Plane;
	Plane.StartTaxi(Plan, UAirsideSettings::ResolveDefaultAirframe());
	const FRouteQuery PlaneQuery = FPlanReResolver::QueryFor(ERouteErrand::Replan,
		FGuidelineNodeId(), FGuidelineNodeId(), Plane);
	TestNull(TEXT("an aircraft's carries no vehicle"), PlaneQuery.Vehicle);
	TestTrue(TEXT("but its wingspan"), PlaneQuery.Wingspan > 0.0);
	return true;
}

#endif
