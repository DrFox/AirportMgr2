#include "Entities/EntityDefinition.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildCodeCStand(Definition);
	return Definition;
}

void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// A Code C contact stand - the A320/737 size that makes up most of a regional airport.
	//
	// ORIGIN is the NOSE GEAR STOP POSITION, not the nose. That is the mark actually
	// painted on a stand and the point a docking guidance system stops the aircraft at, so
	// it is the one place a stand's geometry can be measured from without inventing a datum.
	//
	// LOCAL AXES: +X is the direction the aircraft faces, +Y is its starboard side. Nose-in
	// parking therefore puts the terminal at +X and the aircraft body aft along -X.
	//
	// UNITS are uu, and this project's uu is a centimetre - a real taxiway is 2300 uu wide,
	// which is 23 m. So metres * 100.
	//
	// PROVENANCE, stated honestly: these are derived from published A320 dimensions and
	// standard ramp practice, NOT transcribed from a specific airport's stand drawing.
	// Reference points used - overall length 37.6 m, nose gear 5.1 m aft of the nose,
	// forward hold door ~11.6 m and aft hold door ~24 m aft of the nose, both STARBOARD;
	// passenger doors port. The ones to distrust first are the lateral standoffs, which
	// vary with the vehicle and the airport's own safety envelope.
	auto AddAnchor = [Definition](const TCHAR* Id, double X, double Y, double HeadingDegrees,
		EServiceRole Role)
	{
		FEntityAnchor Anchor;
		Anchor.Id = FName(Id);
		Anchor.LocalPosition = FVector2D(X, Y);
		Anchor.LocalHeading = FMath::DegreesToRadians(HeadingDegrees);
		Anchor.Role = Role;
		Definition->Anchors.Add(Anchor);
	};

	// Plan extent, on the same axes and the same origin as the anchors. A320 reference
	// figures: 37.6 m long, 35.8 m span with sharklets, nose gear 5.1 m aft of the nose,
	// horizontal stabiliser about 12.4 m across. Wing station is taken at the root, roughly
	// 16 m aft of the nose, which is where the span reads correctly in plan.
	Definition->Footprint.NoseX = 507.0;
	Definition->Footprint.TailX = -3250.0;
	Definition->Footprint.Wingspan = 3580.0;
	Definition->Footprint.WingX = -1100.0;
	Definition->Footprint.TailplaneSpan = 1240.0;
	Definition->Footprint.TailplaneX = -2950.0;

	// The aircraft itself: nose gear on the mark, facing the terminal.
	AddAnchor(TEXT("Aircraft"), 0.0, 0.0, 0.0, EServiceRole::Aircraft);

	// Pushback tug, ahead of the nose gear and facing the aircraft. It couples to the nose
	// gear, so it stands off just enough to approach it.
	AddAnchor(TEXT("Tug"), 900.0, 0.0, 180.0, EServiceRole::Tug);

	// Ground power, off the port bow. The A320's external power receptacles are on the
	// forward fuselage underside, port of the centreline.
	AddAnchor(TEXT("GPU"), 250.0, -450.0, 90.0, EServiceRole::GPU);

	// Belt loaders at the two hold doors, both STARBOARD on this type. Positions are the
	// doors' distance aft of the NOSE, less the 5.1 m from nose to nose gear.
	//   forward hold  11.6 m aft of nose -> 6.5 m aft of the gear
	//   aft hold      24.0 m aft of nose -> 18.9 m aft of the gear
	// Facing the fuselage, so heading -90 degrees from the aircraft's.
	AddAnchor(TEXT("BaggageFwd"), -650.0, 500.0, -90.0, EServiceRole::Baggage);
	AddAnchor(TEXT("BaggageAft"), -1890.0, 500.0, -90.0, EServiceRole::Baggage);

	// Refuelling under the starboard wing. The A320's pressure refuelling panel is in the
	// starboard wing leading edge, roughly 17 m aft of the nose; the bowser stands outboard
	// of the fuselage but inboard of the wingtip.
	AddAnchor(TEXT("Fuel"), -1200.0, 850.0, -90.0, EServiceRole::Fuel);

	// Catering at the forward starboard galley door, which is the one that takes the truck
	// on a short turnaround.
	AddAnchor(TEXT("CateringFwd"), -100.0, 550.0, -90.0, EServiceRole::Crew);

	// Passenger steps at the forward port door. On a contact stand this is the air bridge;
	// the anchor is where steps would stand when it is not.
	AddAnchor(TEXT("PassengerFwd"), -50.0, -500.0, 90.0, EServiceRole::Passenger);
}

bool UEntityDefinition::HasUsableAnchorIds(const UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return false;
	}

	TSet<FName> Seen;
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		if (Anchor.Id.IsNone() || Seen.Contains(Anchor.Id))
		{
			return false;
		}
		Seen.Add(Anchor.Id);
	}
	return true;
}

void UEntityDefinition::BuildFootprintLines(
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

	// The envelope - the "outside lines". A rectangle rather than a silhouette because
	// that is what it is FOR: the box the aircraft occupies, which is what a stand's
	// painted boundary encloses and what tells you at a glance whether a service vehicle
	// is parked under a wing or out on the grass.
	Segment(FVector2D(Footprint.NoseX, -HalfSpan), FVector2D(Footprint.NoseX, HalfSpan));
	Segment(FVector2D(Footprint.TailX, -HalfSpan), FVector2D(Footprint.TailX, HalfSpan));
	Segment(FVector2D(Footprint.NoseX, -HalfSpan), FVector2D(Footprint.TailX, -HalfSpan));
	Segment(FVector2D(Footprint.NoseX, HalfSpan), FVector2D(Footprint.TailX, HalfSpan));

	// And the aircraft inside it, as three lines. Enough to read as an aeroplane in plan
	// and to place the wing - which is the thing most of the service positions are
	// measured against - without drawing a silhouette nobody asked for.
	Segment(FVector2D(Footprint.NoseX, 0.0), FVector2D(Footprint.TailX, 0.0));
	Segment(FVector2D(Footprint.WingX, -HalfSpan), FVector2D(Footprint.WingX, HalfSpan));
	Segment(FVector2D(Footprint.TailplaneX, -HalfTail), FVector2D(Footprint.TailplaneX, HalfTail));
}
