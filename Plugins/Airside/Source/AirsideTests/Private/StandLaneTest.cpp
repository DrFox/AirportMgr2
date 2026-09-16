#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneCarriesItsAnchorsTest,
	"Airside.Entities.StandLaneCarriesItsAnchors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneCarriesItsAnchorsTest::RunTest(const FString& Parameters)
{
	// THE WHOLE REDESIGN IN ONE ASSERTION. The ring ran outboard of the wingtips and every
	// anchor hung off it on a spur, which needed a 90 degree turn in 990 uu of depth against
	// the 1399 a truck's steering lock demands. The lane now runs ALONG the row the boxes are
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
	// "a definition may have no lane at all" is a live contract FServiceLoopBuild branches on.
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
	// by 278 uu and neither delivered its radius. This is the clamp FServiceLoopBuild applies
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

#endif
