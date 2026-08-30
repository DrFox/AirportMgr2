#include "Entities/AircraftType.h"

#define LOCTEXT_NAMESPACE "RoadNet"

namespace
{
	/** Add a service point in aircraft-local space. Headings are degrees for legibility. */
	void AddPoint(UAircraftType* Type, const TCHAR* Id, double X, double Y,
		double HeadingDegrees, EServiceRole Role)
	{
		FEntityAnchor Point;
		Point.Id = FName(Id);
		Point.LocalPosition = FVector2D(X, Y);
		Point.LocalHeading = FMath::DegreesToRadians(HeadingDegrees);
		Point.Role = Role;
		Type->ServicePoints.Add(Point);
	}
}

void UAircraftType::BuildFootprintLines(
	const FEntityFootprint& Footprint, TArray<FVector2D>& OutSegments)
{
	OutSegments.Reset();
	if (!Footprint.IsSet())
	{
		return;
	}

	auto Segment = [&OutSegments](const FVector2D& From, const FVector2D& To)
	{
		OutSegments.Add(From);
		OutSegments.Add(To);
	};

	const double HalfSpan = Footprint.Wingspan * 0.5;
	const double HalfTail = Footprint.TailplaneSpan * 0.5;

	// The envelope - the box the aircraft occupies. A rectangle rather than a silhouette
	// because that is what it is FOR: telling you at a glance whether a service vehicle is
	// parked under a wing or out on the grass.
	Segment(FVector2D(Footprint.NoseX, -HalfSpan), FVector2D(Footprint.NoseX, HalfSpan));
	Segment(FVector2D(Footprint.TailX, -HalfSpan), FVector2D(Footprint.TailX, HalfSpan));
	Segment(FVector2D(Footprint.NoseX, -HalfSpan), FVector2D(Footprint.TailX, -HalfSpan));
	Segment(FVector2D(Footprint.NoseX, HalfSpan), FVector2D(Footprint.TailX, HalfSpan));

	// And the aircraft inside it as three lines - enough to read as an aeroplane in plan
	// and to place the wing, which is what most service positions are measured against.
	Segment(FVector2D(Footprint.NoseX, 0.0), FVector2D(Footprint.TailX, 0.0));
	Segment(FVector2D(Footprint.WingX, -HalfSpan), FVector2D(Footprint.WingX, HalfSpan));
	Segment(FVector2D(Footprint.TailplaneX, -HalfTail), FVector2D(Footprint.TailplaneX, HalfTail));
}

bool UAircraftType::HasUsableServiceIds(const UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return false;
	}

	TSet<FName> Seen;
	for (const FEntityAnchor& Point : Type->ServicePoints)
	{
		if (Point.Id.IsNone() || Seen.Contains(Point.Id))
		{
			return false;
		}
		Seen.Add(Point.Id);
	}
	return true;
}

void UAircraftType::BuildA320(UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return;
	}

	Type->Code = TEXT("C");
	Type->DisplayName = LOCTEXT("A320", "A320-200");
	Type->ServicePoints.Reset();

	// 37.6 m long, 35.8 m span with sharklets, nose gear 5.1 m aft of the nose, horizontal
	// stabiliser about 12.4 m across. Origin is the nose gear, so the nose is +507.
	Type->Footprint.NoseX = 507.0;
	Type->Footprint.TailX = -3250.0;
	Type->Footprint.Wingspan = 3580.0;
	Type->Footprint.WingX = -1100.0;
	Type->Footprint.TailplaneSpan = 1240.0;
	Type->Footprint.TailplaneX = -2950.0;

	// Cargo doors are STARBOARD on this type; passenger doors port. Stations are measured
	// aft of the NOSE in the published figures, so each is that less the 5.1 m to the gear.
	//   forward hold  11.6 m aft of nose -> 6.5 m aft of the gear
	//   aft hold      24.0 m aft of nose -> 18.9 m aft of the gear
	AddPoint(Type, TEXT("HoldFwd"), -650.0, 200.0, -90.0, EServiceRole::Baggage);
	AddPoint(Type, TEXT("HoldAft"), -1890.0, 200.0, -90.0, EServiceRole::Baggage);

	// Pressure refuelling panel, starboard wing leading edge, about 17 m aft of the nose.
	AddPoint(Type, TEXT("RefuelPanel"), -1200.0, 400.0, -90.0, EServiceRole::Fuel);

	// External power receptacles, forward fuselage underside, port of the centreline.
	AddPoint(Type, TEXT("GroundPower"), 250.0, -200.0, 90.0, EServiceRole::GPU);

	// Nose gear - where the tug couples.
	AddPoint(Type, TEXT("NoseGear"), 0.0, 0.0, 180.0, EServiceRole::Tug);

	// Doors. L1 is the bridge or the steps; R1 is the galley the caterer works.
	AddPoint(Type, TEXT("DoorL1"), -50.0, -200.0, 90.0, EServiceRole::Passenger);
	AddPoint(Type, TEXT("DoorR1"), -100.0, 200.0, -90.0, EServiceRole::Crew);
}

void UAircraftType::Build737(UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return;
	}

	Type->Code = TEXT("C");
	Type->DisplayName = LOCTEXT("B738", "737-800");
	Type->ServicePoints.Reset();

	// 39.5 m long - LONGER than the A320 despite sharing its code letter - 35.8 m span with
	// winglets, nose gear about 5.2 m aft of the nose.
	Type->Footprint.NoseX = 520.0;
	Type->Footprint.TailX = -3430.0;
	Type->Footprint.Wingspan = 3580.0;
	Type->Footprint.WingX = -1250.0;
	Type->Footprint.TailplaneSpan = 1400.0;
	Type->Footprint.TailplaneX = -3150.0;

	// And the holds sit further aft than the A320's, which is the whole point of this type
	// existing as a separate asset: a belt loader placed for one is wrong for the other.
	//   forward hold  13.2 m aft of nose -> 8.0 m aft of the gear
	//   aft hold      27.5 m aft of nose -> 22.3 m aft of the gear
	AddPoint(Type, TEXT("HoldFwd"), -800.0, 190.0, -90.0, EServiceRole::Baggage);
	AddPoint(Type, TEXT("HoldAft"), -2230.0, 190.0, -90.0, EServiceRole::Baggage);

	AddPoint(Type, TEXT("RefuelPanel"), -1400.0, 380.0, -90.0, EServiceRole::Fuel);
	AddPoint(Type, TEXT("GroundPower"), 200.0, -190.0, 90.0, EServiceRole::GPU);
	AddPoint(Type, TEXT("NoseGear"), 0.0, 0.0, 180.0, EServiceRole::Tug);
	AddPoint(Type, TEXT("DoorL1"), -100.0, -190.0, 90.0, EServiceRole::Passenger);
	AddPoint(Type, TEXT("DoorR1"), -150.0, 190.0, -90.0, EServiceRole::Crew);
}

#undef LOCTEXT_NAMESPACE
