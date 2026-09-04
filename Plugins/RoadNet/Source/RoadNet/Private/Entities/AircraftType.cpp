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

	// 37 m of aeroplane pivoting about a nose gear 12 m ahead of the mains does not change
	// direction quickly, and a tiller is worked gently because the mains track well inside
	// the nose. Roughly a third of the Piper's rate, which is the whole reason this is on
	// the type: the same corner is a different manoeuvre for the two.
	Type->Taxi.TaxiSpeed = 800.0;
	Type->Taxi.MinTaxiSpeed = 50.0;
	Type->Taxi.MaxTurnRateDegPerSec = 8.0;
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

	// Longer than the A320 and steered the same way, so it turns no faster.
	Type->Taxi.TaxiSpeed = 800.0;
	Type->Taxi.MinTaxiSpeed = 50.0;
	Type->Taxi.MaxTurnRateDegPerSec = 8.0;
}

void UAircraftType::BuildPiperMeridian(UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return;
	}

	Type->Code = TEXT("A");
	Type->DisplayName = LOCTEXT("PA46", "PA-46-500TP Meridian");
	Type->ServicePoints.Reset();

	// ORIGIN IS THE MAIN-GEAR AXLE, and this type alone deviates from the nose-gear origin
	// the class documents. Two reasons, both about not writing down a number twice:
	//
	//   SM_PiperMeridian was imported about the main-gear axle - see Tools/Python/
	//   import_piper.py, which MEASURES the mesh rather than trusting the exporter's README.
	//   ARoadAgentActor::SetPose puts that origin on the guideline, so the mains track the
	//   painted line, which is what a taxiing aircraft does.
	//
	//   The figures below are that same measurement, so the drawn envelope and the drawn
	//   aeroplane cannot disagree. Authoring them about the nose gear instead would have
	//   put the envelope a wheelbase - about 2.6 m - ahead of the aircraft inside it.
	//
	// WHAT THIS COSTS: parking one on a stand. A stand's origin is the nose gear stop mark,
	// so composing the two needs the wheelbase, which the other types do not. Nothing does
	// that yet; the day something does, that offset belongs here as a field and not as a
	// constant at the call site.
	Type->Footprint.NoseX = 385.1;
	Type->Footprint.TailX = -531.5;
	Type->Footprint.Wingspan = 1311.0;   // 43 ft 0 in, published, and the import asserts it

	// The mains are ON the wing, which is why the axle sits essentially under the spar.
	Type->Footprint.WingX = 0.0;

	// Tailplane is the one estimate here rather than a measurement - it is used only to
	// draw, and nothing decides anything from it.
	Type->Footprint.TailplaneSpan = 460.0;
	Type->Footprint.TailplaneX = -470.0;

	// NO SERVICE POINTS, deliberately. A Meridian's cabin door and refuel points would be
	// invented numbers - nothing in this repo measures them - and an invented door station
	// is exactly the class of error the provenance note at the top of this class exists to
	// keep out. Nothing consumes them yet either. HasUsableServiceIds passes on an empty
	// array, which is the honest answer: no ids, no duplicate ids.

	Type->Taxi = PiperMeridianTaxi();
}

FTaxiPerformance UAircraftType::PiperMeridianTaxi()
{
	FTaxiPerformance Taxi;

	// TURN RATE, derived rather than dialled in.
	//
	// Steerable nose gear, about +/-15 degrees on the rudder pedals, giving a minimum turn
	// radius of roughly 8 m measured to the outboard wingtip. Nobody takes a turn that
	// tight faster than about 5 kt, which is 2.6 m/s. A turn is v/R:
	//
	//     2.6 / 8 = 0.33 rad/s = 19 deg/s, called 20.
	//
	// So it is two published figures and one piece of ramp practice. If it looks wrong on
	// screen, the thing to argue with is one of those three rather than this number.
	Taxi.MaxTurnRateDegPerSec = 20.0;

	// 10 m/s is 19 kt: fast for a real taxi, and the speed this project has used since the
	// cube. Left alone so this change is about the TURN and nothing else.
	Taxi.TaxiSpeed = 1000.0;

	// 0.8 m/s, about 1.6 kt - a slow walk. Idle thrust against brakes. See FTaxiPerformance:
	// the floor exists because an aeroplane cannot yaw while stopped.
	Taxi.MinTaxiSpeed = 80.0;

	return Taxi;
}

#undef LOCTEXT_NAMESPACE
