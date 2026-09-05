#include "Entities/AircraftType.h"

#define LOCTEXT_NAMESPACE "Airside"

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
	Type->Ground.Taxi.SpeedCap = 800.0;
	Type->Ground.MinTaxiSpeed = 50.0;
	Type->Ground.MaxTurnRateDegPerSec = 8.0;

	// Seventy tonnes moving on idle thrust: it gets under way slowly and is braked gently,
	// because a firm application on a taxiway throws the cabin about.
	Type->Ground.Taxi.Accel = 50.0;
	Type->Ground.Taxi.Decel = 150.0;
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
	Type->Ground.Taxi.SpeedCap = 800.0;
	Type->Ground.MinTaxiSpeed = 50.0;
	Type->Ground.MaxTurnRateDegPerSec = 8.0;
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

	Type->Ground = PiperMeridianGround();
	Type->Climb = PiperMeridianClimb();
	Type->Approach = PiperMeridianApproach();
	Type->Engine = PiperMeridianEngine();
}

FClimbPerformance UAircraftType::PiperMeridianClimb()
{
	FClimbPerformance Climb;

	// The wing's angle at rotation speed. Everything about when the aircraft leaves the
	// ground follows from this and the acceleration - see FClimbPerformance.
	Climb.LiftAngleAtRotateDegrees = 8.0;

	// 120 KIAS, best rate of climb.
	Climb.ClimbSpeed = 6200.0;

	// CHOSEN SO THE PUBLISHED CLIMB RATE COMES OUT, rather than the rate being set directly:
	//
	//   at Vy the wing needs 8 x (4400/6200)^2 = 4.0 degrees
	//   1540 fpm at 120 KIAS is a flight path of asin(780/6200) = 7.2 degrees
	//   so the attitude held is 7.2 + 4.0 = 11.2
	//
	// The rate is then a measurement - Airside.Model.TakeoffRun reads it back and checks it
	// against 780 - instead of a number that agrees with the brochure because it was copied
	// from it.
	Climb.ClimbPitchDegrees = 11.2;

	// About three seconds from nose-down to the climb attitude, which is what a rotation is.
	Climb.RotateRateDegPerSec = 4.0;

	// 300 m. High enough to be clearly departing, low enough to still be worth watching.
	Climb.ClearAltitude = 30000.0;

	return Climb;
}

FGroundPerformance UAircraftType::PiperMeridianGround()
{
	FGroundPerformance Ground;

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
	Ground.MaxTurnRateDegPerSec = 20.0;

	// 10 m/s is 19 kt: fast for a real taxi, and the speed this project has used since the
	// cube. Left alone so this change is about the TURN and nothing else.
	Ground.Taxi.SpeedCap = 1000.0;

	// 0.8 m/s, about 1.6 kt - a slow walk. Idle thrust against brakes. See FGroundPerformance:
	// the floor exists because an aeroplane cannot yaw while stopped.
	Ground.MinTaxiSpeed = 80.0;

	// COMFORTABLE figures, not maximum-performance ones. A Meridian's PT6A can shove it
	// along far harder than this and its brakes can stop it far shorter - on dry concrete
	// the tyres give out somewhere near 6 m/s2 - but nobody taxis an aeroplane like that,
	// and the thing being modelled here is a pilot moving about an airport.
	//
	//   1.0 m/s2 up: rest to taxi speed in ten seconds.
	//   2.0 m/s2 down: a twenty-five metre stop from taxi speed.
	//
	// Down is the larger because wheel brakes beat a propeller, which is true of every
	// aeroplane and is why FGroundRegime keeps the two apart rather than carrying one rate.
	Ground.Taxi.Accel = 100.0;
	Ground.Taxi.Decel = 200.0;

	// TAKE-OFF, derived from two published figures rather than dialled in.
	//
	//   rotation ~85 KIAS      = 44 m/s
	//   sea-level ground roll  = about 1000 ft, 305 m
	//
	// THE PUBLISHED ROLL IS TO LIFT-OFF, NOT TO Vr, and an earlier version of this read it as
	// the latter - which understated the acceleration, because it gave the rotation no
	// distance at all. The nose comes up at Vr and the wheels stay down for another second
	// and a half of accelerating, which is about 75 m of the 305:
	//
	//   a = v2 / 2s = 44 x 44 / (2 x 230) = 4.2 m/s2
	//
	// Four times what it taxis at. The total roll to lift-off is measured back out in
	// Airside.Model.TakeoffRun, which is what makes this a derivation rather than a guess.
	Ground.Takeoff.Accel = 420.0;
	Ground.Takeoff.SpeedCap = 4400.0;

	// A rejected take-off is braking hard from near Vr - harder than anything taxiing asks
	// for, and the one time a light aircraft uses its brakes in anger.
	Ground.Takeoff.Decel = 400.0;

	// LANDING. SpeedCap is Vref, 85 KIAS at maximum landing weight, the same 4400 uu/s the
	// take-off rotates at - which is a coincidence of this airframe, not a rule, and is why
	// the two are separate fields.
	Ground.Landing.SpeedCap = 4400.0;

	// Wheel braking with beta. Derived from the published ground roll of 1,020 ft (31,100 uu)
	// from touchdown, taken to taxi speed rather than to a stop:
	//
	//     a = (v2 - u2) / 2s = (4400^2 - 1000^2) / (2 x 31100) = 295
	//
	// called 300. The MEASURED roll comes out shorter than 31,100 because this model touches
	// down slower than Vref - the flare bleeds it - and Airside.Model.LandingRun reads that
	// back rather than asserting the brochure figure it was derived from.
	Ground.Landing.Decel = 300.0;

	// Idle thrust once the wheels are down and the brakes are released. Small: an aircraft
	// that has slowed below taxi speed still has to keep rolling to steer.
	Ground.Landing.Accel = 100.0;

	return Ground;
}

FEnginePerformance UAircraftType::PiperMeridianEngine()
{
	FEnginePerformance Engine;

	// A PT6A turns its propeller at 2000 RPM governed, which is where the disc threshold in
	// UAirsideAgentAnim came from.
	Engine.MaxRPM = 2000.0;

	// A free-turbine start is unhurried - the gas generator comes up first and the propeller
	// follows it.
	Engine.SpoolUpSeconds = 4.0;

	// Longer, because nothing is driving it: the blades go to feather and windmill down. See
	// FEnginePerformance for why the asymmetry is the point rather than an oversight.
	Engine.SpoolDownSeconds = 9.0;

	return Engine;
}

FApproachPerformance UAircraftType::PiperMeridianApproach()
{
	FApproachPerformance Approach;

	// The standard everywhere, and the one number here that is not about this airframe.
	Approach.GlideslopeDegrees = 3.0;

	// 100 m, so the approach is joined about 1.9 km out. Chosen to mirror ClearAltitude's
	// reasoning from the other end: far enough to read as an approach, near enough to watch.
	Approach.FinalAltitude = 10000.0;

	// Thirty feet, where a light twin's pilot starts raising the nose.
	Approach.FlareHeight = 900.0;

	// About a second and a half from the approach attitude to the flare attitude.
	Approach.FlareRateDegPerSec = 3.0;

	// The limit, not a target. Eight degrees is the angle this wing needs at Vref, so the
	// nose stops where the wing would be exactly holding the aircraft up at its approach
	// speed - beyond that it would be flying it back into the air.
	Approach.MaxFlarePitchDegrees = 8.0;

	// About 1.7 kt/s, which is roughly seven knots across a four-second flare. See
	// FApproachPerformance::FlareDecel for why this is emphatically not the braking figure.
	Approach.FlareDecel = 90.0;

	return Approach;
}

#undef LOCTEXT_NAMESPACE
