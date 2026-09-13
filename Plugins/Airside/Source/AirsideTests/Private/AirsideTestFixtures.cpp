#include "AirsideTestFixtures.h"

#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/LandingRun.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

FAirsideTestWorld::FAirsideTestWorld()
{
	World = UWorld::CreateWorld(EWorldType::Game, false);
	if (World == nullptr) { return; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	Actor = World->SpawnActor<ARoadNetworkActor>();
}

FAirsideTestWorld::~FAirsideTestWorld()
{
	if (World == nullptr) { return; }
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
}

FAirframe TestAirframes::Piper()
{
	FAirframe A;
	A.Ground = UAircraftType::PiperMeridianGround();
	A.Climb = UAircraftType::PiperMeridianClimb();
	A.Approach = UAircraftType::PiperMeridianApproach();
	A.Engine = UAircraftType::PiperMeridianEngine();
	return A;
}

FAirframe TestAirframes::Van()
{
	FAirframe A;
	A.Ground.MaxTurnRateDegPerSec = 90.0;
	return A;
}

FAirframe TestAirframes::GroundOnly()
{
	FAirframe A = UAirsideSettings::ResolveDefaultAirframe();
	A.Climb = FClimbPerformance();
	return A;
}

FRunwayRequirements TestAirframes::PiperRequirements()
{
	return UAircraftType::PiperMeridianRequirements();
}

FGuidelineNodeId TestGraph::Node(URoadNetwork& Net, double X, double Y)
{
	return Net.AddGuidelineNode(FVector2D(X, Y), /*bDerived=*/false);
}

FGuidelineEdgeId TestGraph::Join(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, const FJoinOptions& Options)
{
	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = Options.Control != nullptr
		? *Options.Control
		: (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
	Edge.AllowedTraffic = FTrafficMask::All();
	Edge.Direction = Options.Direction;
	Edge.bDerived = Options.bDerived;
	return Net.AddGuidelineEdge(MoveTemp(Edge));
}

FRoadSegmentId TestGraph::Lay(URoadNetwork& Net, FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile)
{
	return Net.AddStraightSegment(A, B, Profile);
}

FGuidelineNodeId TestGraph::NodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
{
	const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
		{
			return Net.GuidelineNodeIdAt(Index);
		}
	}
	return FGuidelineNodeId();
}

void TestGraph::Rebuild(URoadNetwork& Net)
{
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
	FRoadGuidelineBuilder::Build(Net, Solved);
	FAnchorLink::Build(Net);
}

FTestAirport FTestAirport::Build(const FAirframe& Airframe, const FTestAirportOptions& Options, URoadNetwork* ExistingNet)
{
	FTestAirport Out;
	Out.Net = ExistingNet != nullptr ? ExistingNet : NewObject<URoadNetwork>(GetTransientPackage());
	Out.Threshold = FVector2D::ZeroVector;

	// SIZED FROM THE AIRCRAFT, not chosen - see ArrivalDispatchTest's own comment: a strip
	// shorter than the landing distance is correctly refused, so a fixture that picked a
	// length out of the air would test the refusal or the acceptance depending on numbers
	// nobody was watching.
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const FVector2D Exit1At(Needed * 1.2, 0.0);
	const FVector2D FarAt(Needed * 3.0, 0.0);

	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	const FRoadNodeId ThresholdNode = Out.Net->AddNode(Out.Threshold);

	// THE EXIT STANDS SIT BESIDE: the only one, on a single-exit airport, or the second of two
	// - the shape ArrivalPlannerTest's "earliest exit wins" needs, where BOTH exits reach the
	// one stand and the earlier one must still win despite its longer taxi.
	FVector2D StandExitAt = Exit1At;
	if (Options.ExitCount >= 2)
	{
		const FVector2D Exit2At(Needed * 2.0, 0.0);
		const FRoadNodeId Exit1Node = Out.Net->AddNode(Exit1At);
		const FRoadNodeId Exit2Node = Out.Net->AddNode(Exit2At);
		const FRoadNodeId FarNode = Out.Net->AddNode(FarAt);
		Out.ThresholdSegment = TestGraph::Lay(*Out.Net, ThresholdNode, Exit1Node, Runway);
		TestGraph::Lay(*Out.Net, Exit1Node, Exit2Node, Runway);
		TestGraph::Lay(*Out.Net, Exit2Node, FarNode, Runway);

		// Exit 1's taxiway runs to a dead end - no stand on it directly; exit 2's is the one
		// the stand(s) sit beside. The crossbar joins them so a route exists from EITHER exit,
		// with the one from exit 2 unambiguously the shorter taxi.
		const FRoadNodeId Taxi1End = Out.Net->AddNode(Exit1At + FVector2D(0.0, -Options.TaxiwayLength));
		TestGraph::Lay(*Out.Net, Exit1Node, Taxi1End, Taxiway);
		const FRoadNodeId Taxi2End = Out.Net->AddNode(Exit2At + FVector2D(0.0, -Options.TaxiwayLength));
		TestGraph::Lay(*Out.Net, Exit2Node, Taxi2End, Taxiway);
		TestGraph::Lay(*Out.Net, Taxi1End, Taxi2End, Taxiway);

		StandExitAt = Exit2At;
	}
	else
	{
		const FRoadNodeId ExitNode = Out.Net->AddNode(Exit1At);
		const FRoadNodeId FarNode = Out.Net->AddNode(FarAt);
		Out.ThresholdSegment = TestGraph::Lay(*Out.Net, ThresholdNode, ExitNode, Runway);
		TestGraph::Lay(*Out.Net, ExitNode, FarNode, Runway);

		const FRoadNodeId TaxiEnd = Out.Net->AddNode(Exit1At + FVector2D(0.0, -Options.TaxiwayLength));
		TestGraph::Lay(*Out.Net, ExitNode, TaxiEnd, Taxiway);
	}
	Out.ExitAt = StandExitAt;

	if (Options.bDerived)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);
	}

	// STANDS FACE EAST (heading 0) so their lead-in casts WEST and meets the taxiway - see
	// FAnchorLink's own comment on why a stand must face the guideline it joins. Spaced 6000
	// uu apart, the StandOcc* fixtures' own spacing, so two stands never overlap.
	for (int32 Index = 0; Index < Options.StandCount; ++Index)
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FVector2D StandAt = StandExitAt + FVector2D(9000.0, -10000.0 - 6000.0 * Index);
		Out.Stands.Add(Out.Net->PlaceEntity(Stand, Stand->Anchors, StandAt, 0.0));
	}

	if (Options.bDerived && Options.StandCount > 0)
	{
		FAnchorLink::Build(*Out.Net);
	}

	return Out;
}

URoadProfile* TestProfiles::Runway()
{
	URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Profile->bContinuousThroughJunctions = true;
	return Profile;
}

URoadProfile* TestProfiles::NarrowRunway()
{
	URoadProfile* Profile = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Profile->bContinuousThroughJunctions = true;
	return Profile;
}

URoadProfile* TestProfiles::Taxiway()
{
	return URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
}

FCrossingFixture FCrossingFixture::Build(URoadNetwork& Net, bool bFarBar)
{
	FCrossingFixture Out;
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
	Out.Strip = Net.AddStraightSegment(RA, RB, Runway);

	Out.S = TestGraph::Node(Net, 0.0, -20000.0);
	Out.H = TestGraph::Node(Net, 0.0, -3000.0);
	Out.X = TestGraph::Node(Net, 0.0, 0.0);
	Out.N = TestGraph::Node(Net, 0.0, 20000.0);

	TestGraph::Join(Net, Out.S, Out.H);
	TestGraph::Join(Net, Out.H, Out.X);
	Net.SetRunwayHoldingPositionForTest(Out.H, Out.Strip);

	if (bFarBar)
	{
		// A SECOND BAR ON THE FAR SIDE, protecting the SAME runway - one bar each side, the
		// way a crossing is actually painted, and the shape CrossingHoldsRunway needs to
		// measure that the far bar does not re-arm the crossing once passed.
		const FGuidelineNodeId Far = TestGraph::Node(Net, 0.0, 3000.0);
		TestGraph::Join(Net, Out.X, Far);
		TestGraph::Join(Net, Far, Out.N);
		Net.SetRunwayHoldingPositionForTest(Far, Out.Strip);
	}
	else
	{
		TestGraph::Join(Net, Out.X, Out.N);
	}

	return Out;
}
