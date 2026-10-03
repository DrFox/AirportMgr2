#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Build/AirsideDerivation.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/LandingRun.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

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
	// THE NEAREST THE ROAD NODE when several carry this Origin: since 2026-09-29 an intermediate
	// hold realised at a strip edge is a split node derived FOR the same end (RoadGuidelineBuilder's
	// IntermediateHoldNode), further down the arm. The END is the one at the junction.
	const FRoadSegment* Road = Net.GetSegment(Segment);
	const FRoadNode* RoadEnd = Road ? Net.GetNode(bEndA ? Road->A : Road->B) : nullptr;
	FGuidelineNodeId Best;
	double BestDistance = TNumericLimits<double>::Max();
	const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
		{
			const double Distance = RoadEnd ? FVector2D::Distance(Nodes[Index].Position, RoadEnd->Position) : 0.0;
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Net.GuidelineNodeIdAt(Index);
			}
		}
	}
	return Best;
}

void TestGraph::LayServiceRoads(URoadNetwork& Net, TConstArrayView<TPair<FVector2D, FVector2D>> Pieces)
{
	URoadNetwork* Scratch = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeServiceRoadTransient();

	// ONE ROAD NODE PER END POSITION, in both networks, so pieces that share an end meet at a junction.
	struct FEnd { FVector2D At; FRoadNodeId InScratch; FRoadNodeId InNet; };
	TArray<FEnd> Ends;
	auto EndAt = [&](const FVector2D& At) -> const FEnd&
	{
		for (const FEnd& End : Ends)
		{
			if (End.At.Equals(At, 1.0)) { return End; }
		}
		return Ends.Add_GetRef({ At, Scratch->AddNode(At), Net.AddNode(At) });
	};
	TMap<FRoadSegmentId, FRoadSegmentId> Segments;
	TMap<FRoadNodeId, FRoadNodeId> RoadNodes;
	for (const TPair<FVector2D, FVector2D>& Piece : Pieces)
	{
		const FEnd A = EndAt(Piece.Key);
		const FEnd B = EndAt(Piece.Value);
		RoadNodes.Add(A.InScratch, A.InNet);
		RoadNodes.Add(B.InScratch, B.InNet);
		Segments.Add(Scratch->AddStraightSegment(A.InScratch, B.InScratch, Profile),
			Net.AddStraightSegment(A.InNet, B.InNet, Profile));
	}

	Derive(*Scratch);

	auto MapEnd = [&Segments](FGuidelineEndRef Ref)
	{
		if (Ref.Segment.IsSet()) { Ref.Segment = Segments.FindRef(Ref.Segment); }
		return Ref;
	};
	TMap<FGuidelineNodeId, FGuidelineNodeId> Nodes;
	const TArray<FGuidelineNode>& ScratchNodes = Scratch->GetGuidelineNodes();
	for (int32 Index = 0; Index < ScratchNodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = ScratchNodes[Index];
		if (!Node.bAlive) { continue; }
		const FGuidelineNodeId Copy = Net.AddGuidelineNode(Node.Position, Node.bDerived);
		if (Node.Origin.Segment.IsSet())
		{
			Net.SetGuidelineNodeOrigin(Copy, MapEnd(Node.Origin));
		}
		Nodes.Add(Scratch->GuidelineNodeIdAt(Index), Copy);
	}
	for (const FGuidelineEdge& Edge : Scratch->GetGuidelineEdges())
	{
		if (!Edge.bAlive) { continue; }
		FGuidelineEdge Copy = Edge;
		Copy.A = Nodes.FindRef(Edge.A);
		Copy.B = Nodes.FindRef(Edge.B);
		if (Edge.DerivedFrom.IsSet()) { Copy.DerivedFrom = Segments.FindRef(Edge.DerivedFrom); }
		if (Edge.AtJunction.IsSet()) { Copy.AtJunction = RoadNodes.FindRef(Edge.AtJunction); }
		Copy.EndRefA = MapEnd(Edge.EndRefA);
		Copy.EndRefB = MapEnd(Edge.EndRefB);
		Net.AddGuidelineEdge(MoveTemp(Copy));
	}
}

FRoadSolveResult TestGraph::Derive(URoadNetwork& Net, const FRoadDesignVehicles* DesignVehicles)
{
	// RESOLVE ONCE if the caller has not already: ARoadNetworkActor::MakeSurfaceSettings resolves
	// one FRoadDesignVehicles and AirsideDerivation::Derive passes THE SAME instance to SolveAll and
	// to FRoadGuidelineBuilder::Build (#190, #438) - so a fixture that means the production sequence
	// does the same, rather than SolveAll(nullptr) (each profile resolving its own) followed
	// by a second, independent ResolveRoadDesignVehicles() for Build. The two happen to answer
	// with the same figures today (URoadProfile::ResolvedDesignBody and
	// UAirsideSettings::ResolveRoadDesignVehicles both read UAirsideSettings::
	// ResolveTierDesignVehicles), but a caller that wants to PROVE that - as
	// ResolvedContentOncePerRebuildTest does - passes its own nullptr through to SolveAll
	// directly instead of coming through here.
	const FRoadDesignVehicles Resolved = DesignVehicles != nullptr ? *DesignVehicles : UAirsideSettings::ResolveRoadDesignVehicles();
	// THE PRODUCTION SEQUENCE ITSELF (#438), not a copy of it: this body used to re-type the
	// solve, "the production sequence's restriction pass too" and the builder, and a pass the
	// presenter gained had to be remembered here. The Graph scope is the derivation up to and
	// including the guideline graph and its stamp - no links, which Rebuild adds. NO DefaultProfile:
	// a bare network has no actor to resolve one, and a null one keeps the network's own.
	AirsideDerivation::FDeriveInputs Inputs;
	Inputs.Scope = AirsideDerivation::EDeriveScope::Graph;
	Inputs.DesignVehicles = &Resolved;
	return AirsideDerivation::Derive(Net, Inputs);
}

void TestGraph::Rebuild(URoadNetwork& Net)
{
	// THE FULL SCOPE, THE ONE ARoadNetworkActor::RebuildMesh RUNS (#438): SOLVED, PASSED DOWN (issue
	// #324) to the anchor links along with the rest - without it, a fixture built through this facade
	// could never reproduce a split turn path getting re-measured, only one built by hand around
	// Derive and FAnchorLink::Build directly could. The anchor linker takes the same resolved
	// vehicles' Default (UAirsideSettings::ResolveLargestServiceVehicle's chassis, via
	// ResolveLargestServiceBody) the actor's MakeSurfaceSettings hands it.
	// ENFORCED BY: Airside.Build.Derivation.TestGraphMatchesTheActor
	const FRoadDesignVehicles Resolved = UAirsideSettings::ResolveRoadDesignVehicles();
	AirsideDerivation::FDeriveInputs Inputs;
	Inputs.Scope = AirsideDerivation::EDeriveScope::Full;
	Inputs.DesignVehicles = &Resolved;
	AirsideDerivation::Derive(Net, Inputs);
}

void TestGraph::Link(URoadNetwork& Net)
{
	// See the header: the derivation's tail over a hand-laid graph, with the vehicles Rebuild uses.
	const FRoadDesignVehicles Resolved = UAirsideSettings::ResolveRoadDesignVehicles();
	AirsideDerivation::FDeriveInputs Inputs;
	Inputs.Scope = AirsideDerivation::EDeriveScope::Links;
	Inputs.DesignVehicles = &Resolved;
	AirsideDerivation::Derive(Net, Inputs);
}

TestGraph::FCornerFixture TestGraph::Corner(URoadProfile* Profile, URoadProfile* SecondProfile,
	const FVector2D& CornerAt, const FVector2D& FarAt)
{
	FCornerFixture Out;
	Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId West = Out.Net->AddNode(FVector2D(0.0, 0.0));
	Out.Corner = Out.Net->AddNode(CornerAt);
	const FRoadNodeId Far = Out.Net->AddNode(FarAt);
	Out.First = Out.Net->AddStraightSegment(West, Out.Corner, Profile);
	Out.Second = Out.Net->AddStraightSegment(Out.Corner, Far, SecondProfile != nullptr ? SecondProfile : Profile);
	Out.Solved = Derive(*Out.Net);
	return Out;
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
		Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const FVector2D Exit1At(Needed * 1.2, 0.0);
	const FVector2D FarAt(Needed * 3.0, 0.0);

	// No FTestAirportOptions knob for this: nothing has ever needed a taxiway other length
	// than the StandOcc* fixtures' own 20000, so there is nothing yet to name a parameter for.
	constexpr double TaxiwayLength = 20000.0;

	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	const FRoadNodeId ThresholdNode = Out.Net->AddNode(Out.Threshold);

	// THE EXIT STANDS SIT BESIDE: the only one, on a single-exit airport, or the second of two
	// - the shape ArrivalPlannerTest's "earliest exit wins" needs, where BOTH exits reach the
	// one stand and the earlier one must still win despite its longer taxi.
	Out.Exits.Add(Exit1At);
	FVector2D StandExitAt = Exit1At;
	if (Options.ExitCount >= 2)
	{
		const FVector2D Exit2At(Needed * 2.0, 0.0);
		Out.Exits.Add(Exit2At);
		const FRoadNodeId Exit1Node = Out.Net->AddNode(Exit1At);
		const FRoadNodeId Exit2Node = Out.Net->AddNode(Exit2At);
		const FRoadNodeId FarNode = Out.Net->AddNode(FarAt);
		Out.ThresholdSegment = TestGraph::Lay(*Out.Net, ThresholdNode, Exit1Node, Runway);
		TestGraph::Lay(*Out.Net, Exit1Node, Exit2Node, Runway);
		TestGraph::Lay(*Out.Net, Exit2Node, FarNode, Runway);

		// Exit 1's taxiway runs to a dead end - no stand on it directly; exit 2's is the one
		// the stand(s) sit beside. The crossbar joins them so a route exists from EITHER exit,
		// with the one from exit 2 unambiguously the shorter taxi.
		const FRoadNodeId Taxi1End = Out.Net->AddNode(Exit1At + FVector2D(0.0, -TaxiwayLength));
		TestGraph::Lay(*Out.Net, Exit1Node, Taxi1End, Taxiway);
		const FRoadNodeId Taxi2End = Out.Net->AddNode(Exit2At + FVector2D(0.0, -TaxiwayLength));
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

		const FRoadNodeId TaxiEnd = Out.Net->AddNode(Exit1At + FVector2D(0.0, -TaxiwayLength));
		TestGraph::Lay(*Out.Net, ExitNode, TaxiEnd, Taxiway);
	}
	Out.ExitAt = StandExitAt;

	if (Options.bDerived)
	{
		TestGraph::Derive(*Out.Net);
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
		// THE DERIVATION'S TAIL (#438) over the graph Derive laid above, now the stands stand beside it.
		TestGraph::Link(*Out.Net);
	}

	return Out;
}

FGuidelineNodeId FTestAirport::Pose(FEntityInstanceId Stand) const
{
	const FEntityInstance* E = Net->GetEntity(Stand);
	return E != nullptr ? E->PoseNode : FGuidelineNodeId();
}

FGuidelineNodeId FTestTwoRunways::Pose(int32 Index) const
{
	const FEntityInstance* E = Stands.IsValidIndex(Index) ? Net->GetEntity(Stands[Index]) : nullptr;
	return E != nullptr ? E->PoseNode : FGuidelineNodeId();
}

void FTestTwoRunways::SetUse(FRoadSegmentId Seed, ERunwayUse Use) const
{
	FRunwayFacts Facts = Net->RunwayFactsFor(Seed);
	Facts.Use = Use;
	Net->SetRunwayFacts(Seed, Facts);
}

void FTestTwoRunways::Hold(FTrafficOccupancy& Occupancy, FRoadSegmentId Seed, int32 AgentId) const
{
	for (const FTrafficResource& Surface : Net->RunwaySurfaces(Seed))
	{
		Occupancy.Assert(FTrafficClaim::Make(AgentId, Surface, /*bOccupied*/ true, 2));
	}
}

FTestTwoRunways FTestTwoRunways::Build(const FAirframe& Airframe, URoadNetwork* ExistingNet)
{
	FTestTwoRunways Out;
	Out.Net = ExistingNet != nullptr ? ExistingNet : NewObject<URoadNetwork>(GetTransientPackage());
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const double ExitX = Needed * 1.2;
	const double FarX = Needed * 3.0;
	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	auto Strip = [&](double Y, FRoadNodeId& OutExit)
	{
		const FRoadNodeId Threshold = Out.Net->AddNode(FVector2D(0.0, Y));
		OutExit = Out.Net->AddNode(FVector2D(ExitX, Y));
		const FRoadNodeId Far = Out.Net->AddNode(FVector2D(FarX, Y));
		const FRoadSegmentId Seed = TestGraph::Lay(*Out.Net, Threshold, OutExit, Runway);
		TestGraph::Lay(*Out.Net, OutExit, Far, Runway);
		return Seed;
	};
	FRoadNodeId ExitA, ExitB;
	Out.A = Strip(0.0, ExitA);
	Out.B = Strip(-40000.0, ExitB);
	const FRoadNodeId Middle = Out.Net->AddNode(FVector2D(ExitX, -20000.0));
	TestGraph::Lay(*Out.Net, ExitA, Middle, Taxiway);
	TestGraph::Lay(*Out.Net, Middle, ExitB, Taxiway);
	TestGraph::Derive(*Out.Net);

	// FACING EAST, so the lead-in casts west onto the taxiway - FTestAirport::Build's reason.
	for (const double Y : { -10000.0, -16000.0 })
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.Stands.Add(Out.Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(ExitX + 9000.0, Y), 0.0));
	}
	// THE DERIVATION'S TAIL (#438) - FTestAirport::Build's reason.
	TestGraph::Link(*Out.Net);
	return Out;
}

FTestAirport FTestAirport::BuildScale(const FAirframe& Airframe, int32 Seed, bool bDerived, URoadNetwork* ExistingNet)
{
	FTestAirport Out;
	Out.Net = ExistingNet != nullptr ? ExistingNet : NewObject<URoadNetwork>(GetTransientPackage());
	FRandomStream Stream(Seed);

	// SIZED FROM THE AIRCRAFT, same reasoning as Build() above - a strip that could not
	// actually land the airframe would test the wrong thing at any scale.
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;

	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	// TWO RUNWAYS, each split at two exits into three segments - Build()'s own single-exit
	// shape, doubled, not a new one: the point of this fixture is scale, not a new topology
	// for a budget test to have to learn.
	const double RunwayLength = Needed * 4.0;
	const double Exit1AtX = Needed * 1.2;
	const double Exit2AtX = Needed * 2.5;
	constexpr double RunwaySeparation = 260000.0; // clears the whole grid below with room over

	auto LayRunwayWithExits = [&](double Y, FRoadNodeId& OutExit1) -> void
	{
		const FRoadNodeId W = Out.Net->AddNode(FVector2D(0.0, Y));
		const FRoadNodeId E1 = Out.Net->AddNode(FVector2D(Exit1AtX, Y));
		const FRoadNodeId E2 = Out.Net->AddNode(FVector2D(Exit2AtX, Y));
		const FRoadNodeId E = Out.Net->AddNode(FVector2D(RunwayLength, Y));
		TestGraph::Lay(*Out.Net, W, E1, Runway);
		TestGraph::Lay(*Out.Net, E1, E2, Runway);
		TestGraph::Lay(*Out.Net, E2, E, Runway);
		OutExit1 = E1;
	};

	FRoadNodeId Runway1Exit1, Runway2Exit1;
	LayRunwayWithExits(0.0, Runway1Exit1);
	LayRunwayWithExits(-RunwaySeparation, Runway2Exit1);
	Out.Threshold = FVector2D::ZeroVector;
	Out.ExitAt = FVector2D(Exit1AtX, 0.0);
	Out.Exits.Add(Out.ExitAt);

	// THE GRID: an 8x20 lattice of taxiway nodes between the two runways - 8*19 + 20*7 = 292
	// segments, plus the 6 above and the 2 connectors below, ~300 in total (see the issue's
	// own "~300-segment" - this is not tuned to hit the figure exactly, only to be the same
	// order of magnitude a built-out airport reaches). NOT randomised: only which nodes carry
	// a stand, a depot, or an agent's endpoint varies with Seed - the lattice itself is the
	// same shape every call, so a segment-count assertion never depends on the seed either.
	constexpr int32 GridRows = 8;
	constexpr int32 GridCols = 20;
	constexpr double GridDX = 6000.0;
	constexpr double GridDY = 6000.0;
	constexpr double GridOriginY = -60000.0; // south of runway 1, well clear of its own pavement

	TArray<FRoadNodeId> GridNodes;
	GridNodes.SetNum(GridRows * GridCols);
	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			GridNodes[Row * GridCols + Col] = Out.Net->AddNode(
				FVector2D(Col * GridDX, GridOriginY - Row * GridDY));
		}
	}
	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		for (int32 Col = 0; Col < GridCols - 1; ++Col)
		{
			TestGraph::Lay(*Out.Net, GridNodes[Row * GridCols + Col], GridNodes[Row * GridCols + Col + 1], Taxiway);
		}
	}
	for (int32 Row = 0; Row < GridRows - 1; ++Row)
	{
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			TestGraph::Lay(*Out.Net, GridNodes[Row * GridCols + Col], GridNodes[(Row + 1) * GridCols + Col], Taxiway);
		}
	}

	// TWO CONNECTORS, each dropped from its own runway's exit to the grid COLUMN nearest
	// that exit's own X, so the spur meets its runway close to perpendicular rather than at
	// a shallow diagonal across most of the grid's width - a route from the exit into the
	// grid still shouldn't have to detour sideways first. Independent of the ear-clip
	// warning at these two exit junctions (RoadMeshBuilder's "rim not star-shaped from any
	// apex" fallback): that fires from the WIDTH difference between a 4500 uu runway and a
	// 2300 uu taxiway meeting at a T, not from this angle - moving the connector's target
	// column measurably changes nothing about it (checked). A test that wants the census line
	// absent at this scale reads past that warning by naming "Rebuilt:", which is ScaleFixtureTest.cpp's
	// header's note, rather than papering over it here.
	const int32 ConnectorCol = FMath::Clamp(FMath::RoundToInt(Exit1AtX / GridDX), 0, GridCols - 1);
	TestGraph::Lay(*Out.Net, Runway1Exit1, GridNodes[ConnectorCol], Taxiway);
	TestGraph::Lay(*Out.Net, Runway2Exit1, GridNodes[(GridRows - 1) * GridCols + ConnectorCol], Taxiway);

	if (bDerived)
	{
		TestGraph::Derive(*Out.Net);
	}

	// 30 STANDS ALONG ROW 0 (nearest runway 1), NORTH of it, facing NORTH (heading +90 deg) so
	// the lead-in casts SOUTH and meets the row - the same heading FuelDepotAnchorTest proved
	// for a guideline south of an entity. Spread evenly across the row's own span with a small
	// seeded jitter, so no two stands sit on the same taxiway cell and the layout is not a
	// visibly hand-ruled line of them either.
	constexpr int32 StandCount = 30;
	constexpr double StandOffsetY = 4000.0;
	const double RowSpan = (GridCols - 1) * GridDX;
	// CLAMPED, NOT LEFT TO THE JITTER ALONE: row 0's own taxiway edges only exist between
	// X=0 and X=RowSpan, so the two interpolated endpoints (Index 0 and StandCount-1, whose
	// baseline X already sits exactly on the row's own ends) must not jitter PAST either end
	// - a straight-down lead-in from a point west of X=0 or east of RowSpan crosses no edge
	// at all and joins nothing. A 1000 uu margin keeps every stand's ray over the row.
	for (int32 Index = 0; Index < StandCount; ++Index)
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const double Baseline = (RowSpan * Index) / (StandCount - 1);
		const double X = FMath::Clamp(Baseline + Stream.FRandRange(-1500.0, 1500.0), 1000.0, RowSpan - 1000.0);
		const FVector2D StandAt(X, GridOriginY + StandOffsetY);
		Out.Stands.Add(Out.Net->PlaceEntity(Stand, Stand->Anchors, StandAt, UE_DOUBLE_PI * 0.5));
	}

	// 4 FUEL DEPOTS ALONG THE LAST ROW (nearest runway 2), SOUTH of it, facing SOUTH (heading
	// -90 deg) so the lead-in casts NORTH and meets the row - the mirror of the stands above.
	// PoseRole and Trucks come from the depot definition, same as FuelDepotAnchorTest: a
	// depot's pose is a vehicle's, not an aircraft's, and PlaceEntity cannot read that off the
	// definition itself (Model/ must not depend on Entities/ - see PlaceEntity's own comment).
	constexpr int32 DepotCount = 4;
	constexpr double DepotOffsetY = 4000.0;
	const int32 LastRow = GridRows - 1;
	for (int32 Index = 0; Index < DepotCount; ++Index)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		const double Baseline = (RowSpan * Index) / (DepotCount - 1);
		const double X = FMath::Clamp(Baseline + Stream.FRandRange(-1500.0, 1500.0), 1000.0, RowSpan - 1000.0);
		const FVector2D DepotAt(X, GridOriginY - LastRow * GridDY - DepotOffsetY);
		Out.Depots.Add(Out.Net->PlaceEntity(Depot, Depot->Anchors, DepotAt,
			-UE_DOUBLE_PI * 0.5, /*DesignWingspan=*/0.0, Depot->PoseRole, Depot->Trucks));
	}

	if (bDerived)
	{
		// THE DERIVATION'S TAIL (#438) - FTestAirport::Build's reason.
		TestGraph::Link(*Out.Net);
	}

	return Out;
}

URoadProfile* TestProfiles::Runway()
{
	URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Profile->bContinuousThroughJunctions = true;
	// EMPTY, as every runway profile asset leaves it (all four) - Fill laid the road list.
	Profile->AllowedPavements.Reset();
	return Profile;
}

URoadProfile* TestProfiles::NarrowRunway()
{
	URoadProfile* Profile = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Profile->bContinuousThroughJunctions = true;
	// EMPTY, as every runway profile asset leaves it (all four) - Fill laid the road list.
	Profile->AllowedPavements.Reset();
	return Profile;
}

URoadProfile* TestProfiles::Taxiway()
{
	return URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
}

FRoutePlan TestGraph::Probe(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
	ETraversalClass Class, const FVehicle* Vehicle, double Wingspan)
{
	FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Wingspan, Class);
	if (Vehicle != nullptr)
	{
		Query.WithVehicle(*Vehicle);
	}
	return RouteSearch::Find(Net, Query);
}

TArray<URoadProfile*> TestProfiles::ServiceTiers()
{
	TArray<URoadProfile*> Out;
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	// WideServiceTier + 1, not a bare 3: the guard NAMES why three tiers are required (Wide
	// sits at index WideServiceTier, so the array needs one more slot than that index).
	if (Content == nullptr || Content->ServiceRoadProfiles.Num() != UAirsideSettings::WideServiceTier + 1)
	{
		return Out;
	}
	for (const TSoftObjectPtr<URoadProfile>& Tier : Content->ServiceRoadProfiles)
	{
		Out.Add(Tier.LoadSynchronous());
	}
	return Out;
}

#endif // WITH_DEV_AUTOMATION_TESTS
