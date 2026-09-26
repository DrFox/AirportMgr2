#include "AirsideTestFixtures.h"

#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideLabel.h"

// FAirsideTestWorld's constructor/destructor now live in Testing/AirsideTestWorld.h (#189) -
// AirsideTestFixtures.h forwards to that header rather than declaring its own copy.

FToolContext TestTool::ContextAt(IRoadEditTarget& Target, const FVector2D& Where,
	ERoadSnapKind Kind, double SnapRadius)
{
	FToolContext Context;
	Context.Target = &Target;
	Context.SnapRadius = SnapRadius;

	FRoadSnapResult Snap;
	Snap.Kind = Kind;
	Snap.Position = Where;
	Context.SetCursor(Where, Snap);
	return Context;
}

FAirframe TestAirframes::Piper()
{
	FAirframe A;
	A.Chassis.Ground = UAircraftType::PiperMeridianGround();
	A.Climb = UAircraftType::PiperMeridianClimb();
	A.Approach = UAircraftType::PiperMeridianApproach();
	A.Engine = UAircraftType::PiperMeridianEngine();
	return A;
}

FVehicle TestAirframes::Van()
{
	FVehicle A;
	A.Chassis.Ground.MaxTurnRateDegPerSec = 90.0;
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

UAircraftType* TestAirframes::PiperType()
{
	UAircraftType* Type = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildPiperMeridian(Type);
	return Type;
}

// TestGraph::Node/Join/Lay/NodeFor/Derive/Rebuild/Corner, TestProfiles::* and FTestAirport::
// Build/BuildScale/Pose now live in Airside/Private/Testing/AirsideTestGraph.cpp (#311) -
// this file's own AirsideTestFixtures.h forwards their declarations from the Public header
// that replaces them, Testing/AirsideTestGraph.h.

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

FExitArcAirport ExitArcBuildAirport(UObject* Outer, bool bWithStand, double XDistance)
{
	FExitArcAirport Out;
	Out.XAt = FVector2D(-40000.0 + XDistance, 0.0);
	Out.Net = NewObject<URoadNetwork>(Outer);
	URoadProfile* Runway = TestProfiles::NarrowRunway();
	Runway->ExitLength = Out.ExitLength;
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId W = Out.Net->AddNode(Out.Threshold);
	const FRoadNodeId X = Out.Net->AddNode(Out.XAt);
	const FRoadNodeId E = Out.Net->AddNode(FVector2D(FMath::Max(60000.0, Out.XAt.X + 40000.0), 0.0));
	const FRoadNodeId T = Out.Net->AddNode(Out.XAt + FVector2D(20000.0, -20000.0));
	Out.RW1 = Out.Net->AddStraightSegment(W, X, Runway);
	Out.RW2 = Out.Net->AddStraightSegment(X, E, Runway);
	Out.XT = Out.Net->AddStraightSegment(X, T, Taxiway);
	TestGraph::Derive(*Out.Net);
	if (bWithStand)
	{
		// Faces east (heading 0), so its lead-in casts WEST and meets the 45 degree
		// taxiway at (34000, -14000), 11000 uu away - inside FAnchorLink's reach.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.XAt + FVector2D(25000.0, -14000.0), 0.0);
		FAnchorLink::Build(*Out.Net, UAirsideSettings::ResolveLargestServiceVehicle());
	}
	return Out;
}

FGuidelineNodeId ExitArcNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
{
	const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = Nodes[Index];
		if (Node.bAlive && Node.Origin.Segment == Segment && Node.Origin.bEndA == bEndA
			&& Node.Origin.GuidelineIndex == 0)
		{
			return Net.GuidelineNodeIdAt(Index);
		}
	}
	return FGuidelineNodeId();
}

FGuidelineNodeId ExitArcNodeNear(const URoadNetwork& Net, const FVector2D& At, double& OutMiss)
{
	const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
	FGuidelineNodeId Best;
	OutMiss = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].bAlive) { continue; }
		const double Miss = FVector2D::Distance(Nodes[Index].Position, At);
		if (Miss < OutMiss)
		{
			OutMiss = Miss;
			Best = Net.GuidelineNodeIdAt(Index);
		}
	}
	return Best;
}

const FGuidelineEdge* ExitArcTurnBetween(const URoadNetwork& Net, FGuidelineNodeId P, FGuidelineNodeId Q)
{
	for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
		if ((Edge.A == P && Edge.B == Q) || (Edge.A == Q && Edge.B == P))
		{
			return &Edge;
		}
	}
	return nullptr;
}

namespace TestGuide
{
/** An anchor with no reference and no points, so ONLY the network sources answer. */
FGuideAnchor BareAnchor(const FVector2D& Origin)
{
	FGuideAnchor Anchor;
	Anchor.Origin = Origin;
	return Anchor;
}

/**
 * A runway strip. NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a
 * runway is not a road kind - it is a segment placed through PlaceRunway with a runway
 * profile, which is what URoadNetwork::IsRunwaySegment then recognises.
 *
 * Minimum is dropped first: MinimumRunwayLength defaults to 50000 uu and PlaceRunway refuses
 * anything under it, so a test strip either lowers the bar or is half a kilometre long.
 * MeshFreshnessTest does exactly this, for exactly this reason.
 */
bool LayRunway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To, double Minimum)
{
	// A NODE FIRST, PURELY TO BRING THE NETWORK INTO BEING. The facade creates URoadNetwork
	// lazily inside PlaceNode and PlaceRunway does NOT - so a test whose first call is
	// PlaceRunway leaves Actor->Network null, and dereferencing it reads offset 0x60 off a
	// null pointer. That is not hypothetical: it crashed this very test, and a crash hides
	// its cause where a failure would have named it. MeshFreshnessTest places a node first
	// for the same reason and says so.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

	URoadProfile* Profile = TestProfiles::Runway();

	Actor->MinimumRunwayLength = Minimum;
	return Actor->PlaceRunway(From, To, Profile);
}

/**
 * Every candidate ONE source proposes, with the rest of the chain kept out of it.
 *
 * DESCRIPTION IS FILLED HERE, NOT BY Source.Propose - #183 moved that out of every source and
 * into FSnapGuideChain::Resolve, which this helper bypasses on purpose (it exists to test ONE
 * source's raw output, arbitration and all). A test that reads Candidate.Description - most of
 * NetworkGuideSourceTest does - would otherwise see an empty string forever; filling it here,
 * the same way Resolve does for its winners, keeps every such test unchanged and still measuring
 * what it always measured, while production Propose stays free of the allocation.
 */
TArray<SnapGuide::FCandidate> ProposedBy(const IGuideSource& Source,
	const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const TOptional<FVector2D>& Cursor)
{
	TArray<SnapGuide::FCandidate> Out;
	Source.Propose(Network, Anchor, Cursor.Get(Anchor.Origin), Out);
	for (SnapGuide::FCandidate& Candidate : Out)
	{
		Candidate.Description = SnapGuide::Describe(Network, Anchor, Candidate.Label);
	}
	return Out;
}
}
