#include "AirsideTestFixtures.h"

#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/AircraftType.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"

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
