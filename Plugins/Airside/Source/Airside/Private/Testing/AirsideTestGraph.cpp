#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/LandingRun.h"
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

FRoadSolveResult TestGraph::Derive(URoadNetwork& Net, const FRoadDesignVehicles* DesignVehicles, EWideningTrace Widening)
{
	// RESOLVE ONCE if the caller has not already: URoadSurfacePresenter::Rebuild resolves its
	// own FRoadDesignVehicles and passes THE SAME instance to SolveAll and to
	// FRoadGuidelineBuilder::Build (#190) - so a fixture that means the production sequence
	// does the same, rather than SolveAll(nullptr) (each profile resolving its own) followed
	// by a second, independent ResolveRoadDesignVehicles() for Build. The two happen to answer
	// with the same figures today (URoadProfile::ResolvedDesignBody and
	// UAirsideSettings::ResolveRoadDesignVehicles both read UAirsideSettings::
	// ResolveTierDesignVehicles), but a caller that wants to PROVE that - as
	// ResolvedContentOncePerRebuildTest does - passes its own nullptr through to SolveAll
	// directly instead of coming through here.
	const FRoadDesignVehicles Resolved = DesignVehicles != nullptr ? *DesignVehicles : UAirsideSettings::ResolveRoadDesignVehicles();
	FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net, 12, &Resolved, Widening);
	FRoadGuidelineBuilder::Build(Net, Solved, Resolved);
	return Solved;
}

void TestGraph::Rebuild(URoadNetwork& Net)
{
	Derive(Net);
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
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
		FAnchorLink::Build(*Out.Net, UAirsideSettings::ResolveLargestServiceVehicle());
	}

	return Out;
}

FGuidelineNodeId FTestAirport::Pose(FEntityInstanceId Stand) const
{
	const FEntityInstance* E = Net->GetEntity(Stand);
	return E != nullptr ? E->PoseNode : FGuidelineNodeId();
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

	// AN INTERIOR NODE, away from every grid edge (row/col 2 of an 8x20 grid, never row/col 0
	// or the last one), for a drag test that wants an ordinary junction - see SampleGridNode's
	// own comment.
	Out.SampleGridNode = GridNodes[2 * GridCols + 2];

	// TWO CONNECTORS, each dropped from its own runway's exit to the grid COLUMN nearest
	// that exit's own X, so the spur meets its runway close to perpendicular rather than at
	// a shallow diagonal across most of the grid's width - a route from the exit into the
	// grid still shouldn't have to detour sideways first. Independent of the ear-clip
	// warning at these two exit junctions (RoadMeshBuilder's "rim not star-shaped from any
	// apex" fallback): that fires from the WIDTH difference between a 4500 uu runway and a
	// 2300 uu taxiway meeting at a T, not from this angle - moving the connector's target
	// column measurably changes nothing about it (checked). See
	// Airside.Perf.Scale.DragFrameStaysGeometryOnly's own comment for how that test reads
	// past it rather than papering over it here.
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
		FAnchorLink::Build(*Out.Net, UAirsideSettings::ResolveLargestServiceVehicle());
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
