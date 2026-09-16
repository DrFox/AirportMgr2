#include "CoreMinimal.h"
#include "Build/StandLaneBuild.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "StandFixture.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneCarriesItsAnchorsTest,
	"Airside.Entities.StandLaneCarriesItsAnchors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneCarriesItsAnchorsTest::RunTest(const FString& Parameters)
{
	// THE WHOLE REDESIGN IN ONE ASSERTION. The ring ran outboard of the wingtips and every
	// anchor hung off it on a spur, which needs a 90 degree turn: 989.1 uu of run on each of
	// its two arms, both cut into the same straight, so 1978 uu against the 990 uu of depth
	// there was between the ring and the box row. The lane now runs ALONG the row the boxes are
	// painted on, so the anchors are ON it and nothing turns into one.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand))
	{
		return false;
	}

	TSet<FName> OnLane;
	for (const FStandWaypoint& Point : Stand->ServiceLane)
	{
		if (Point.Kind == EStandWaypointKind::Anchor)
		{
			OnLane.Add(Point.AnchorId);
		}
	}

	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
		{
			continue;
		}
		TestTrue(
			*FString::Printf(TEXT("anchor '%s' is ON the lane, not spurred off it"),
				*Anchor.Id.ToString()),
			OnLane.Contains(Anchor.Id));
	}

	// AND EVERY ANCHOR WAYPOINT SITS WHERE ITS ANCHOR DOES. A waypoint carrying an id but a
	// different position would put the truck beside the box rather than in it, and every
	// other assertion here would still pass.
	for (const FStandWaypoint& Point : Stand->ServiceLane)
	{
		if (Point.Kind != EStandWaypointKind::Anchor)
		{
			continue;
		}
		const FEntityAnchor* Declared = Stand->Anchors.FindByPredicate(
			[&Point](const FEntityAnchor& Candidate) { return Candidate.Id == Point.AnchorId; });
		if (!TestNotNull(
				*FString::Printf(TEXT("waypoint names a real anchor '%s'"),
					*Point.AnchorId.ToString()),
				Declared))
		{
			continue;
		}
		TestTrue(
			*FString::Printf(TEXT("waypoint for '%s' sits on it"), *Point.AnchorId.ToString()),
			Point.Local.Equals(Declared->LocalPosition, 0.5));
	}

	// FOUR ENTRIES, one per side. The ring's header valued "entry from any side" and it is
	// worth keeping; a stand with one entrance is the cul-de-sac AnchorLink's per-side rule
	// exists to prevent.
	int32 Entries = 0;
	for (const FStandWaypoint& Point : Stand->ServiceLane)
	{
		Entries += Point.Kind == EStandWaypointKind::Entry ? 1 : 0;
	}
	TestEqual(TEXT("four entries, one per side"), Entries, 4);

	// A DEPOT HAS NONE, and empty is a supported state rather than an unfinished one. Carried
	// over from Airside.Entities.ServiceLoopEnclosesTheStand, which this file replaces: every
	// other assertion that test made was about the ring's four corners and died with them, but
	// "a definition may have no lane at all" is a live contract FStandLaneBuild branches on.
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (TestNotNull(TEXT("a depot definition"), Depot))
	{
		TestEqual(TEXT("a depot has no service lane - one pose, no anchors"),
			Depot->ServiceLane.Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneCornersClearTheTruckLockTest,
	"Airside.Entities.StandLaneCornersClearTheTruckLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneCornersClearTheTruckLockTest::RunTest(const FString& Parameters)
{
	// MEASURED ON THE CURVE, NEVER ON THE FILLET THAT SHAPED IT. That distinction cost three
	// sessions in 8be494c one level up. Here it is asked of the DEFINITION's polyline: every
	// corner must have legs long enough for CornerRunFor at the radius the largest admitted
	// vehicle needs, with neither neighbour stealing the leg.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
	const double Needed = Largest.TightestFollowableRadius();
	if (!TestTrue(TEXT("the largest service vehicle steers on measured axles"), Needed > 0.0))
	{
		return false;
	}

	const int32 Count = Stand->ServiceLane.Num();
	if (!TestTrue(TEXT("the stand has a lane with corners to check"), Count >= 4))
	{
		return false;
	}

	TArray<double> Run;
	Run.SetNumZeroed(Count);

	for (int32 At = 0; At < Count; ++At)
	{
		const FVector2D& Here = Stand->ServiceLane[At].Local;
		const FVector2D& Previous = Stand->ServiceLane[(At + Count - 1) % Count].Local;
		const FVector2D& Next = Stand->ServiceLane[(At + 1) % Count].Local;

		const FVector2D Back = (Previous - Here).GetSafeNormal();
		const FVector2D Onward = (Next - Here).GetSafeNormal();
		const double Interior = FMath::Acos(
			FMath::Clamp(FVector2D::DotProduct(Back, Onward), -1.0, 1.0));

		// A straight-through waypoint - an anchor sitting mid-run - has no corner to check.
		if (Interior > UE_DOUBLE_PI - 0.01)
		{
			continue;
		}

		Run[At] = GuidelineGeom::CornerRunFor(Needed, Interior);
		const double Shortest = FMath::Min(
			FVector2D::Distance(Here, Previous), FVector2D::Distance(Here, Next));

		TestTrue(
			*FString::Printf(
				TEXT("corner %d (%.0f deg) needs %.0f uu of leg and the shorter leg is %.0f"),
				At, FMath::RadiansToDegrees(Interior), Run[At], Shortest),
			Shortest >= Run[At]);
	}

	// AND THE SUM ON EVERY STRAIGHT, NOT THE LARGER OF ITS TWO ENDS.
	//
	// THE LOOP ABOVE ALONE IS NOT ENOUGH, and finding that out is what reshaped this layout.
	// Two corners cut back into the same straight from opposite ends, so a straight that is
	// long enough for each of them SEPARATELY can still be too short for both: the first draft
	// put two square 90 degree corners, 989 uu of run each, on the 1700 uu straight between the
	// two runs, and every per-corner assertion above passed on it while the two curves overlapped
	// by 278 uu and neither delivered its radius. This is the clamp FStandLaneBuild applies
	// one level down, asked of the definition instead.
	for (int32 At = 0; At < Count; ++At)
	{
		const int32 Next = (At + 1) % Count;
		const double Length =
			FVector2D::Distance(Stand->ServiceLane[At].Local, Stand->ServiceLane[Next].Local);
		TestTrue(
			*FString::Printf(
				TEXT("the straight from %d to %d is %.0f uu and its two corners cut %.0f + %.0f"),
				At, Next, Length, Run[At], Run[Next]),
			Length >= Run[At] + Run[Next]);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneClearsTheAircraftTest,
	"Airside.Entities.StandLaneClearsTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneClearsTheAircraftTest::RunTest(const FString& Parameters)
{
	// THE OLD RING'S ONE REAL PROMISE, KEPT. Airside.Entities.ServiceLoopClearsTheAircraft made
	// this assertion about a lane that ran outboard of the wingtips, where it was nearly free;
	// the lane now runs INSIDE them, along the aeroplane, which is precisely when the promise
	// starts to cost something. So it survives the redesign rather than dying with the ring.
	//
	// The centreline as a finite segment from TailX to NoseX, never the footprint outline:
	// forbidding the footprint would forbid passing UNDER A WING, which is normal and which the
	// hydrant requires - HydrantPit is under the starboard wing root because that is where a
	// hydrant pit is.
	//
	// NO SPURS IN THE COUNT any more, and that is the redesign rather than an omission: an
	// anchor is a waypoint ON the lane now, so there is no spur from one to the other to cross
	// anything.
	auto CountCentrelineCrossings = [](const UEntityDefinition& Definition) -> int32
	{
		if (Definition.DesignAircraft == nullptr || Definition.ServiceLane.Num() < 3)
		{
			return 0;
		}

		const FEntityFootprint& Footprint = Definition.DesignAircraft->Footprint;
		const FVector2D Tail(Footprint.TailX, 0.0);
		const FVector2D Nose(Footprint.NoseX, 0.0);

		int32 Crossings = 0;
		for (int32 At = 0; At < Definition.ServiceLane.Num(); ++At)
		{
			const FVector2D& A = Definition.ServiceLane[At].Local;
			const FVector2D& B =
				Definition.ServiceLane[(At + 1) % Definition.ServiceLane.Num()].Local;
			Crossings += RoadGeom::SegmentsCross(A, B, Tail, Nose) ? 1 : 0;
		}
		return Crossings;
	};

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand)) { return false; }

	TestEqual(TEXT("no lane segment crosses the fuselage lengthwise"),
		CountCentrelineCrossings(*Stand), 0);

	// THE VACUITY CHECK. A counter that could never report anything would pass the assertion
	// above on a stand whose lane ran straight through the aeroplane, so the same counter is
	// pointed at a layout that genuinely does cross.
	//
	// THE CROSSING LAID LEVEL WITH THE TAILPLANE rather than astern of it, hand-built: exactly
	// what the aircraft-footprint clamp in BuildCodeCStandFor exists to prevent, and what the
	// anchors alone would have allowed, since the aft box sits at about -2300.
	{
		UEntityDefinition* Bad = NewObject<UEntityDefinition>(GetTransientPackage());
		UEntityDefinition::BuildCodeCStand(Bad, Stand->DesignAircraft);

		Bad->ServiceLane.Reset();
		const FVector2D Corners[4] = {
			FVector2D( 1745.0,  1100.0), FVector2D( 1745.0, -600.0),
			FVector2D(-2000.0,  -600.0), FVector2D(-2000.0, 1100.0) };
		for (const FVector2D& Corner : Corners)
		{
			FStandWaypoint Point;
			Point.Local = Corner;
			Bad->ServiceLane.Add(Point);
		}

		TestTrue(TEXT("a crossing laid level with the tailplane cuts the fuselage"),
			CountCentrelineCrossings(*Bad) > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneDoesNotFoldThroughItselfTest,
	"Airside.Entities.StandLaneDoesNotFoldThroughItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneDoesNotFoldThroughItselfTest::RunTest(const FString& Parameters)
{
	// THE ONE CONTRACT OF THE DELETED ServiceLoopEnclosesTheStand THAT WAS NOT ABOUT THE RING.
	// That test reached for PointInPolygon rather than a bounds comparison for a stated reason -
	// "a bounds test would still pass if the builder emitted the corners in an order that folds
	// the box through itself" - and when the ring's shape assertions died, that one nearly went
	// with them. It is worth more now: a four-point box could hardly fold, and a seventeen-point
	// cycle with a dip in its upper edge and a bulge at each end very much can.
	//
	// AT TWO RADII, because the shape is DERIVED and a layout that is simple at one vehicle is
	// not thereby simple at another - the dip deepens, the crossings bulge further out, and the
	// boxes march apart, all at different rates. Checking one radius would be checking one
	// arithmetic result, which is the thing this task exists to stop.
	FAirframe Longer = UAirsideSettings::ResolveLargestServiceVehicle();
	Longer.SteerAxleX *= 1.5;

	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	struct FCase
	{
		const TCHAR* Name;
		FAirframe Vehicle;
	};
	const FCase Cases[2] = {
		{ TEXT("the shipping dispenser"), UAirsideSettings::ResolveLargestServiceVehicle() },
		{ TEXT("a vehicle half as long again"), Longer } };

	for (const FCase& Case : Cases)
	{
		UEntityDefinition* Stand = NewObject<UEntityDefinition>(GetTransientPackage());
		UEntityDefinition::BuildCodeCStandFor(Stand, A320, Case.Vehicle);

		const int32 Count = Stand->ServiceLane.Num();
		if (!TestTrue(
				*FString::Printf(TEXT("%s gets a lane with enough points to fold"), Case.Name),
				Count >= 4))
		{
			continue;
		}

		TArray<FVector2D> Outline;
		Outline.Reserve(Count);
		for (const FStandWaypoint& Point : Stand->ServiceLane)
		{
			Outline.Add(Point.Local);
		}

		// THE PAIRWISE LOOP IS HERE TO NAME THE PAIR, not to be the authority. RoadGeom::
		// IsSimplePolygon below is the codebase's one evaluator of this question and the
		// assertion that counts; a bare "it folded" with no segment indices would be diagnosed
		// by hand off a dump, which is exactly what this test replaces.
		//
		// CLOSED IMPLICITLY, so segment At runs from At to (At+1) % Count and the LAST segment
		// is a real one, adjacent to both the first segment and the one before it. Getting that
		// wrong would skip the only two segments the wrap introduces.
		for (int32 At = 0; At < Count; ++At)
		{
			const int32 AEnd = (At + 1) % Count;
			for (int32 Other = At + 1; Other < Count; ++Other)
			{
				const int32 BEnd = (Other + 1) % Count;
				if (AEnd == Other || BEnd == At)
				{
					// Adjacent: they share a vertex by construction and always "touch".
					continue;
				}

				TestFalse(
					*FString::Printf(
						TEXT("%s: segment %d->%d (%.0f,%.0f)-(%.0f,%.0f) crosses %d->%d "
						     "(%.0f,%.0f)-(%.0f,%.0f)"),
						Case.Name, At, AEnd, Outline[At].X, Outline[At].Y,
						Outline[AEnd].X, Outline[AEnd].Y, Other, BEnd,
						Outline[Other].X, Outline[Other].Y, Outline[BEnd].X, Outline[BEnd].Y),
					RoadGeom::SegmentsCross(
						Outline[At], Outline[AEnd], Outline[Other], Outline[BEnd]));
			}
		}

		// AND THE AUTHORITY, which must agree with every TestFalse above. It reports only
		// TRANSVERSAL crossings - a vertex touch and a collinear overlap are documented as not
		// counted - so this is "the cycle does not fold through itself" and not "the cycle is
		// non-degenerate in every way". The corner-run assertions in
		// Airside.Entities.StandLaneCornersClearTheTruckLock are what rule out the degenerate
		// cases, because a zero-length side cannot carry a cut.
		TestTrue(*FString::Printf(TEXT("%s: the closed lane is a simple polygon"), Case.Name),
			RoadGeom::IsSimplePolygon(Outline));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxesMoveWithTheLargestVehicleTest,
	"Airside.Entities.StandBoxesMoveWithTheLargestVehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxesMoveWithTheLargestVehicleTest::RunTest(const FString& Parameters)
{
	// THE DERIVATION, NOT ITS OUTPUT. Asserting EquipmentFwd == -127 would pass just as well
	// against a hand-typed -127, which is the thing this change exists to stop. So: build the
	// stand around a LONGER vehicle and require the boxes to have moved outward.
	//
	// The pit does not move. A hydrant is plant dug into concrete under the wing root - it is
	// the fixed thing the paint is arranged around, not the other way about.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	UEntityDefinition* Normal = NewObject<UEntityDefinition>(GetTransientPackage());
	UEntityDefinition::BuildCodeCStand(Normal, A320);

	FAirframe Longer = UAirsideSettings::ResolveLargestServiceVehicle();
	Longer.SteerAxleX *= 1.5;

	UEntityDefinition* Roomier = NewObject<UEntityDefinition>(GetTransientPackage());
	UEntityDefinition::BuildCodeCStandFor(Roomier, A320, Longer);

	auto XOf = [](const UEntityDefinition* Definition, const TCHAR* Id) -> double
	{
		const FEntityAnchor* Found = Definition->Anchors.FindByPredicate(
			[Id](const FEntityAnchor& Candidate) { return Candidate.Id == FName(Id); });
		return Found != nullptr ? Found->LocalPosition.X : TNumericLimits<double>::Lowest();
	};

	TestTrue(TEXT("a longer vehicle pushes EquipmentFwd forward"),
		XOf(Roomier, TEXT("EquipmentFwd")) > XOf(Normal, TEXT("EquipmentFwd")) + 1.0);
	TestTrue(TEXT("a longer vehicle pushes EquipmentAft aft"),
		XOf(Roomier, TEXT("EquipmentAft")) < XOf(Normal, TEXT("EquipmentAft")) - 1.0);
	TestTrue(TEXT("and the hydrant pit does not move, because concrete does not"),
		FMath::IsNearlyEqual(
			XOf(Roomier, TEXT("HydrantPit")), XOf(Normal, TEXT("HydrantPit")), 0.5));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlacedStandLaneIsOneDrivableCycleTest,
	"Airside.Build.PlacedStandLaneIsOneDrivableCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlacedStandLaneIsOneDrivableCycleTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE DEFINITION'S PROMISES, ASKED OF WHAT THE BUILDER ACTUALLY LAID. Every other test in
	// this file measures UEntityDefinition::ServiceLane - a polyline of straights, which is how
	// a lane is DESCRIBED. This one places the stand and measures the GRAPH, which is what a
	// truck drives: a definition whose legs are long enough can still deliver a tight curve if
	// the builder's proportional clamp shrank a corner to fit its leg. That is the 8be494c
	// lesson, one level up.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FStandLaneBuild::FResult Built = FStandLaneBuild::Build(*Net);
	const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
	if (!TestNotNull(TEXT("the stand got a lane"), Lane))
	{
		return false;
	}

	// NO DEAD END. Forward-only rests entirely on this: a stub is a reverse, and reverse does
	// not exist yet. Every lane node carries two lane edges - the anchors included, which is
	// what "the lane runs THROUGH the box" means and what the ring's spur pairs never had.
	TMap<FGuidelineNodeId, int32> Degree;
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge == nullptr || !Edge->bAlive)
		{
			continue;
		}
		++Degree.FindOrAdd(Edge->A);
		++Degree.FindOrAdd(Edge->B);
	}
	for (const TPair<FGuidelineNodeId, int32>& Node : Degree)
	{
		TestEqual(TEXT("every lane node is driven through, never into"), Node.Value, 2);
	}

	// AND EVERY CURVE IS DRIVABLE, measured on the SAMPLED geometry rather than on the
	// definition's corners - the 8be494c lesson. A definition whose legs are long enough can
	// still deliver a tight curve if the builder's clamp shrank a corner to fit its leg.
	const double Needed =
		UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge == nullptr || !Edge->bAlive)
		{
			continue;
		}
		const FGuidelineNode* A = Net->GetGuidelineNode(Edge->A);
		const FGuidelineNode* B = Net->GetGuidelineNode(Edge->B);
		if (A == nullptr || B == nullptr)
		{
			continue;
		}
		const double Delivered =
			GuidelineGeom::TightestRadius(A->Position, Edge->Control, B->Position);
		TestTrue(
			*FString::Printf(TEXT("a lane curve delivers %.0f uu against the %.0f needed"),
				Delivered, Needed),
			Delivered >= Needed - 0.5);
	}

	// AND NOTHING RUNS THROUGH THE AEROPLANE. The rule is unchanged - under a wing is normal,
	// through the fuselage is not - but the fuselage is a RECTANGLE now, not an axis, because
	// this lane runs alongside it where a zero-width centreline would permit a route down the
	// skin. Measured on the sampled curve, not on the definition's corners.
	{
		const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
		const double HalfWidth = Footprint.FuselageWidth * 0.5;
		const FBox2D Fuselage(
			FVector2D(Footprint.TailX, -HalfWidth), FVector2D(Footprint.NoseX, HalfWidth));
		TestTrue(TEXT("the design aircraft has a fuselage width to test against"),
			Footprint.FuselageWidth > 0.0);

		for (const FGuidelineEdgeId& Id : *Lane)
		{
			TArray<FVector2D> Points;
			if (!Net->SampleGuideline(Id, Points))
			{
				continue;
			}
			for (const FVector2D& Point : Points)
			{
				TestFalse(
					*FString::Printf(TEXT("a lane point at (%.0f,%.0f) is inside the fuselage"),
						Point.X, Point.Y),
					Fuselage.IsInside(Point));
			}
		}
	}

	// THE ANCHORS KEPT THEIR OWN NODES. A lane that made fresh nodes at the anchor positions
	// would look identical here and route nothing: FuelService asks for the ANCHOR's node.
	const FEntityInstance* Entity = Net->GetEntity(Placed);
	if (TestNotNull(TEXT("the stand is placed"), Entity))
	{
		for (const FResolvedAnchor& Anchor : Entity->ResolvedAnchors)
		{
			if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
			{
				continue;
			}
			const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
			if (TestNotNull(
					*FString::Printf(TEXT("anchor '%s' has a node"), *Anchor.Id.ToString()),
					Node))
			{
				TestEqual(
					*FString::Printf(TEXT("and the lane runs through '%s'"),
						*Anchor.Id.ToString()),
					Node->Incident.Num(), 2);
			}
		}
	}

	// AND THE DECLARED ENTRIES REACHED THE GRAPH. Nothing in this task READS FResult::Entries -
	// Task 5 of the stand routing work is its consumer - so without this the map could be empty
	// on every stand and no test above would notice until that task started and blamed itself.
	// Four entries, each a corner and so each rounded into a pair of nodes.
	if (const TArray<FGuidelineNodeId>* Entries = Built.Entries.Find(Placed))
	{
		int32 Declared = 0;
		for (const FStandWaypoint& Point : Stand->ServiceLane)
		{
			Declared += Point.Kind == EStandWaypointKind::Entry ? 1 : 0;
		}
		TestTrue(
			*FString::Printf(TEXT("%d declared entries reached the graph as %d node(s)"),
				Declared, Entries->Num()),
			Declared > 0 && Entries->Num() >= Declared);

		for (const FGuidelineNodeId& Id : *Entries)
		{
			TestTrue(TEXT("and an entry node is a node of the lane itself"),
				Built.Nodes.Contains(Id));
		}
	}
	else
	{
		AddError(TEXT("the stand recorded no entries at all"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneFrozenForASmallerVehicleIsShavedTest,
	"Airside.Build.StandLaneFrozenForASmallerVehicleIsShaved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneFrozenForASmallerVehicleIsShavedTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE ASSET IS FROZEN AND THE RADIUS IS NOT, which is the one way this lane can go bad
	// without anybody editing it. UEntityDefinition::BuildCodeCStandFor sizes every leg from
	// ResolveLargestServiceVehicle() AT AUTHORING TIME and bakes the answer into
	// DA_Stand_CodeC; FStandLaneBuild::Build re-resolves the same call on EVERY rebuild. Admit
	// a bigger dispenser next month and the two disagree - the builder asks each corner for
	// more run than the frozen legs can give, its proportional clamp shaves them, and the
	// corners come out under the lock they exist to clear.
	//
	// NO OTHER TEST CAN SEE THIS. Every one of them builds the definition and the lane from
	// the SAME vehicle, so the two figures agree by construction; the shipped asset is the one
	// place they can drift. Authoring for a SHORTER vehicle and building for the real one is
	// that drift, staged.
	//
	// WHAT THIS PINS IS THE CONDITION THE BUILDER WARNS ON, not the wording of the warning.
	// FStandLaneBuild::MeasureLane logs a Warning naming the stand and the corner whenever a
	// corner as laid comes in under the radius it was sized for; a UE_LOG is not an assertable
	// value here (warnings are not elevated to errors in this project's automation), so what is
	// asserted is the measurable fact the log line reports - and the line itself is read out of
	// Saved/Logs/AirsideTests.log. If this test ever goes green with no such line, the warning
	// has been unwired.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	FAirframe Smaller = UAirsideSettings::ResolveLargestServiceVehicle();
	Smaller.SteerAxleX *= 0.5;

	UEntityDefinition* Frozen = NewObject<UEntityDefinition>(GetTransientPackage());
	UEntityDefinition::BuildCodeCStandFor(Frozen, A320, Smaller);

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FEntityInstanceId Placed = PlaceStand(*Net, *Frozen, FVector2D::ZeroVector, 0.0);

	const FStandLaneBuild::FResult Built = FStandLaneBuild::Build(*Net);
	const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
	if (!TestNotNull(TEXT("the frozen stand still gets a lane"), Lane))
	{
		return false;
	}

	// THE RADIUS THE BUILDER ASKED FOR, which is the live one and not the frozen one.
	const double Needed =
		UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();

	double Tightest = TNumericLimits<double>::Max();
	FVector2D TightAt = FVector2D::ZeroVector;
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge == nullptr || !Edge->bAlive)
		{
			continue;
		}
		const FGuidelineNode* A = Net->GetGuidelineNode(Edge->A);
		const FGuidelineNode* B = Net->GetGuidelineNode(Edge->B);
		if (A == nullptr || B == nullptr)
		{
			continue;
		}
		const double Delivered =
			GuidelineGeom::TightestRadius(A->Position, Edge->Control, B->Position);
		if (Delivered < Tightest)
		{
			Tightest = Delivered;
			TightAt = Edge->Control;
		}
	}

	AddInfo(FString::Printf(
		TEXT("FROZEN: tightest laid corner %.0f uu at (%.0f,%.0f), against the %.0f the live "
		     "vehicle needs"),
		Tightest, TightAt.X, TightAt.Y, Needed));

	// SHAVED, AND BY MORE THAN THE WELD TOLERANCE'S SHARE. The builder's own report threshold
	// is one percent of the radius, so asserting below that is asserting exactly the state it
	// speaks up about rather than a state it tolerates.
	TestTrue(*FString::Printf(
			TEXT("a lane frozen for a smaller vehicle comes out under the lock - %.0f uu "
			     "against %.0f"),
			Tightest, Needed),
		Tightest < Needed * 0.99);

	return true;
}

#endif
