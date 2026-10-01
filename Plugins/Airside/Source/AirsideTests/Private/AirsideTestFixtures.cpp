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

	// THE FLOOR (#292 review finding): this builder is hand-assembled, not routed through
	// FBuildSession::MakeContext, so nothing else resolves FToolContext::Envelopes for it. The
	// floor is what every test in this module that draws a stand ghost already assumes -
	// FloorEnvelopeForLetter equals ResolveLetterEnvelope for every letter with no fleet type
	// modelled past it, which is every automation test's own content set (none).
	Context.Envelopes = FLetterEnvelopeTable::Floor();

	FRoadSnapResult Snap;
	Snap.Kind = Kind;
	Snap.Position = Where;
	Context.SetCursor(Where, Snap);
	return Context;
}

FAirframe TestAirframes::Piper()
{
	// THE MERIDIAN READ THE WAY AN ASSET IS READ (#479): BuildPiperMeridian, then UAircraftType::Airframe() - the
	// mapping the content-less production default uses (UAirsideSettings::ContentlessDefaultAirframe), reached here
	// through PiperType() rather than through the resolver so a content set that names a DefaultAircraft cannot
	// change what 120-odd call sites measure.
	//
	// THIS WAS A HAND COPY, AND A WRONG ONE: the four performance structs and nothing else, so the vehicle every
	// traffic test took had the FChassis default Pivot steer law (every modelled aeroplane rolls on its mains), no
	// wheelbase, a steered final turn, no body centre, no wingspan, TypeCode, requirements or fuel. #477 removed the
	// same drift from production and found six traffic tests measuring the nose where the body centre was meant.
	// ENFORCED BY: Airside.Content.FixturePiperIsTheMeridiansOwn (every FAirframe property, by reflection)
	return PiperType()->Airframe();
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

FRoadCrossingFixture FRoadCrossingFixture::Lay(URoadNetwork& Net, bool bFarSide, double ArmLength)
{
	FRoadCrossingFixture Out;
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	Net.DefaultProfile = Taxiway;

	const FRoadNodeId WestEnd = Net.AddNode(FVector2D(-ArmLength, 0.0));
	Out.Centre = Net.AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId EastEnd = Net.AddNode(FVector2D(ArmLength, 0.0));
	const FRoadNodeId SouthEnd = Net.AddNode(FVector2D(0.0, -ArmLength));

	Out.West = Net.AddStraightSegment(WestEnd, Out.Centre, Taxiway);
	Out.East = Net.AddStraightSegment(Out.Centre, EastEnd, Taxiway);
	Out.South = Net.AddStraightSegment(SouthEnd, Out.Centre, Road);
	if (bFarSide)
	{
		const FRoadNodeId NorthEnd = Net.AddNode(FVector2D(0.0, ArmLength));
		Out.North = Net.AddStraightSegment(Out.Centre, NorthEnd, Road);
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
		// taxiway at (34000, -14000), 13000 uu away - inside FAnchorLink's reach.
		//
		// 27000 EAST, NOT 25000, since the taxiway clearance strip (2026-09-28): at 25000 the
		// box's back corner stood 35 m from the taxiway's dead-end tip, inside its 40 m reach
		// (half-width + strip), and admission closed the only stand. Moved along the lead-in's
		// own line, so where it meets the taxiway is unchanged.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.XAt + FVector2D(27000.0, -14000.0), 0.0);
		// THE DERIVATION'S TAIL (#438), not FAnchorLink::Build typed here: what production runs after the graph.
		TestGraph::Link(*Out.Net);
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

bool TestTool::ConnectUnjudged(ARoadNetworkActor& Actor, int32 FromIndex, int32 ToIndex, ERoadKind Kind)
{
	URoadProfile* Profile = Actor.ResolveProfileFor(Kind, INDEX_NONE);
	FRoadNodeId From, To;
	if (Profile == nullptr || Actor.Network == nullptr
		|| !Actor.MakeLiveNodeId(FromIndex, From) || !Actor.MakeLiveNodeId(ToIndex, To))
	{
		return false;
	}
	const bool bLaid = Actor.Network->AddStraightSegment(From, To, Profile).IsSet();
	Actor.RebuildMesh();
	return bLaid;
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
 * Minimum is dropped first: MinimumRunwayLength defaults to 30000 uu (50000 until 2026-09-27)
 * and PlaceRunway refuses anything under it, so a test strip either lowers the bar or is 300 m long.
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

FRoutePlan TestPlans::Chain(const TArray<FRun>& Runs)
{
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	double Along = 0.0;
	for (const FRun& Run : Runs)
	{
		const int32 From = Plan.Polyline.IsEmpty() ? 0 : 1;
		if (Run.Points.Num() <= From)
		{
			continue;
		}
		for (int32 K = From; K < Run.Points.Num(); ++K)
		{
			if (!Plan.Polyline.IsEmpty())
			{
				Along += FVector2D::Distance(Plan.Polyline.Last(), Run.Points[K]);
			}
			Plan.Polyline.Add(Run.Points[K]);
		}
		FRouteStep Step;
		Step.EndVertex = Plan.Polyline.Num() - 1;
		Step.EndDistance = Along;
		Step.bReverseLeg = Run.bReverse;
		Plan.Steps.Add(Step);
	}
	Plan.Length = Along;
	return Plan;
}
