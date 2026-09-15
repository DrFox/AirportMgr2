#include "Content/AirsideSettings.h"

#include "Content/AirsideContent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Entities/AircraftType.h"

// File-local, matching every other category in this module.
DEFINE_LOG_CATEGORY_STATIC(LogAirsideContent, Log, All);

UAirsideSettings::UAirsideSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Airside");
}

const UAirsideContent* UAirsideSettings::GetContent()
{
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	if (Settings == nullptr || Settings->Content.IsNull())
	{
		// Nothing configured. A supported state: every consumer of this already has to cope
		// with a null default, because each of these assets was optional before it moved
		// here. Deliberately silent - a project that assigns its own materials on the actor
		// should not be nagged about a set it does not want.
		return nullptr;
	}

	const UAirsideContent* Loaded = Settings->Content.LoadSynchronous();
	if (Loaded == nullptr)
	{
		// CONFIGURED AND MISSING is the case worth shouting about, and the reason this
		// function exists rather than a bare LoadSynchronous at each call site. The paths
		// this replaced failed exactly here and said so only in a CDO-time line that
		// scrolled past at startup, hours before anyone noticed the roads were gone.
		//
		// Warned once per load attempt rather than once ever: the useful moment to see it
		// is when the thing that needed it ran, not when the module happened to start.
		UE_LOG(LogAirsideContent, Warning,
			TEXT("Airside content set '%s' is configured but could not be loaded. "
				 "Materials, the default profile and the stand definition will all be absent. "
				 "Check Project Settings > Plugins > Airside."),
			*Settings->Content.ToString());
	}

	return Loaded;
}

FAirframe UAirsideSettings::ResolveDefaultAirframe()
{
	if (const UAirsideContent* Content = GetContent(); Content != nullptr)
	{
		if (const UAircraftType* Default = Content->DefaultAircraft.LoadSynchronous())
		{
			return Default->Airframe();
		}
	}

	// TODO(#30): author DA_PiperMeridian and set UAirsideContent::DefaultAircraft. Until an
	// asset exists to point it at, this is the fallback every project runs on - including
	// every automation test, which configures no content set at all.
	FAirframe Piper;
	Piper.Ground = UAircraftType::PiperMeridianGround();
	Piper.Climb = UAircraftType::PiperMeridianClimb();
	Piper.Approach = UAircraftType::PiperMeridianApproach();
	Piper.Engine = UAircraftType::PiperMeridianEngine();

	// Must agree with the content branch's Default->Airframe(), which reads
	// Footprint.Wingspan - a route search costs a turn by Wingspan (RouteSearch::Find), so
	// a fallback that left this at the FAirframe default (0.0) would let the SAME aircraft
	// take a turn too tight for its own wing depending purely on whether content happened
	// to be loaded.
	Piper.Wingspan = UAircraftType::PiperMeridianWingspan();

	// Same rule as Wingspan: the content branch reads Type->Requirements, so the fallback
	// must carry the Piper's too, or admission would judge the same aircraft by 0 m field
	// lengths (no claim) with content unloaded and by 800 m with it loaded.
	Piper.Requirements = UAircraftType::PiperMeridianRequirements();
	return Piper;
}

FAirframe UAirsideSettings::ResolveDefaultVehicle()
{
	// NO CONTENT LOOKUP, unlike ResolveDefaultAirframe above, and deliberately: there is no
	// UVehicleType asset to point at yet, so a soft pointer here would be a content slot
	// nothing could fill - the "authored numbers nothing reads" failure, one level up. M3
	// adds the type with the fleet, and this function is where it will be resolved.
	FAirframe Van;

	// A LIGHT COMMERCIAL VEHICLE, in FGroundRegime's units (uu/s and uu/s^2; a uu is a
	// centimetre). 1 m/s^2 up, 2 m/s^2 braking, 10 m/s flat out - an airside speed limit
	// rather than a road one. Written out rather than left to the struct defaults because
	// this is where a truck's performance is DECIDED, and a reader must be able to see the
	// figures without opening another header to find out they happen to coincide.
	Van.Ground.Taxi.Accel = 100.0;
	Van.Ground.Taxi.Decel = 200.0;
	Van.Ground.Taxi.SpeedCap = 1000.0;

	// Kept rolling to steer, like an aircraft, for a different reason that lands in the same
	// place: a van CAN pivot, but a follower allowed to stop dead mid-turn would snap its
	// heading round rather than swing it. 0.5 m/s.
	Van.Ground.MinTaxiSpeed = 50.0;

	// NINE TIMES an airframe's 10 deg/s. A van turns into a depot in its own length; an
	// aircraft's rate here would sweep it across the kerb and back.
	//
	// A CEILING NOW, NOT THE LAW, because the axles below turn the geometric law on. It stays
	// as the guard it always was against a rate nothing else bounds.
	Van.Ground.MaxTurnRateDegPerSec = 90.0;

	// MEASURED AXLES, which is what stops the truck PIVOTING.
	//
	// FAirframe::Wheelbase's own comment argues the other way - "a van is authored at 90
	// deg/s and would need 83 degrees of lock at its creep speed, so it is pivoting rather
	// than steering, and a bicycle model would cripple every service vehicle on the airport".
	// That was right about a van NOBODY HAD MEASURED: with no axles the wheelbase is zero,
	// HasAxles() is false, and the flat rate is all there is. Its arithmetic is also exactly
	// the symptom - 90 deg/s at the 50 uu/s creep is a 0.32 m radius, which is not a turn,
	// it is a spin about the rear axle. Reported from play on 2026-09-14: the truck drives up
	// to the stand, stops, swings 90 degrees on the spot and drives off.
	//
	// fueltruck1 HAS been measured. Its rig puts steer_FL/FR at x = 360.7 uu and wheel_RL/RR
	// at the origin, so the wheelbase is 3.607 m and the geometric law has real figures to
	// work with. At the 45 degrees of lock below that is a 3.6 m turn radius for a 6.2 m
	// truck - tight, correct for a rigid bowser, and nothing like crippled.
	//
	// ORIGIN AT THE FIXED AXLE, which is the Piper's declared deviation rather than plane2's
	// nose-gear convention - and it is the model's, not a choice made here: fueltruck1's
	// README fixes its origin at "rear axle centre, projected to the ground, because that is
	// what a front-steered truck pivots about". Airside.Content.VehicleFootprintMatchesTheMesh
	// asserts both figures against the mesh's own bones so they cannot drift from it.
	Van.SteerAxleX = 360.7;
	Van.FixedAxleX = 0.0;

	// 50 degrees, written out rather than left at FGroundRegime's 60. The struct default is
	// an aircraft nose gear's, and a rigid truck does not have that: 60 would give a 2.1 m
	// radius, which is inside the truck's own length and reads as the pivot the axles above
	// removed.
	//
	// WAS 45, RAISED 2026-09-14 with the service road's fillet - the two are one decision.
	// A rigid vehicle cannot follow an arc tighter than Wheelbase / sin(lock) at ANY speed,
	// so 45 asked for a 5.10 m corner while the service road was authored with a 5.00 m one.
	// Ten centimetres apart, on opposite sides of a cliff: every corner between the depot and
	// the stand hit TIGHTER THAN THE STEERING LOCK and crawled at MinTaxiSpeed. 50 asks for
	// 4.71 m, and Airside.Model.ServiceRoadFilletClearsTheTruckLock now holds the pair
	// together so neither can drift into the other again.
	//
	// 50 rather than the 60 that would also have "fixed" it: at 4.71 m this is still a turn
	// a 6.2 m rigid truck plausibly makes, where 2.1 m is a pirouette.
	Van.Ground.MaxSteerDegrees = 50.0;

	// A TRUCK CANNOT FLY, AND SAYS SO. Zeroed rather than left at the struct defaults, which
	// are a light twin's and are all NON-ZERO - so a default-constructed FApproachPerformance
	// answers IsSet() == true, and this van would have passed as landable to anything that
	// asked (ArrivalPlanner and FRoadAgent both branch on exactly that call). Nothing asks
	// today, which is precisely why it is worth making false by construction rather than by
	// nobody having got round to it.
	//
	// One decisive field each is enough: IsSet() is an AND over every figure.
	Van.Approach.GlideslopeDegrees = 0.0;
	Van.Climb.LiftAngleAtRotateDegrees = 0.0;

	// ENGINE IS LEFT ALONE, deliberately, and it is the one exception. FRoadAgent runs the
	// spool-up and spool-down through it and writes EngineRPM into FAgentMotion; zeroing it
	// would make a truck's own motion struct describe an engine that never turns, which is a
	// lie about a running vehicle rather than a refusal to fly. The box does not draw it.
	//
	// What the inspector SAYS this is - see FAirframe::TypeCode. FUEL rather than VAN
	// because the panel names the job the player can see, and this slice has exactly one.
	Van.TypeCode = TEXT("FUEL");

	return Van;
}

FResolvedAgentView UAirsideSettings::ResolveAgentView(const FAirframe& Airframe)
{
	const UAirsideContent* Content = GetContent();

	FResolvedAgentView View;
	View.Mesh = Airframe.Mesh.LoadSynchronous();
	View.AnimClass = Airframe.AnimClass.LoadSynchronous();
	if (View.Mesh == nullptr && Content != nullptr)
	{
		View.Mesh = Content->AgentMesh.LoadSynchronous();
		View.AnimClass = Content->AgentAnimClass.LoadSynchronous();
	}
	else if (View.Mesh != nullptr && View.AnimClass == nullptr && Content != nullptr)
	{
		// A type with a mesh but no anim Blueprint: the default one drives bones by name, so
		// it is the right fallback rather than no animation at all.
		View.AnimClass = Content->AgentAnimClass.LoadSynchronous();
	}
	return View;
}

UStaticMesh* UAirsideSettings::ResolveVehicleMesh()
{
	const UAirsideContent* Content = GetContent();
	return Content != nullptr ? Content->VehicleMesh.LoadSynchronous() : nullptr;
}

FResolvedAgentView UAirsideSettings::ResolveVehicleView()
{
	const UAirsideContent* Content = GetContent();

	FResolvedAgentView View;
	if (Content != nullptr)
	{
		View.Mesh = Content->VehicleSkeletalMesh.LoadSynchronous();
		// ONLY WITH A MESH. An anim class on its own has nothing to drive, and letting it
		// through would make the caller's "did this resolve?" test - View.Mesh != nullptr -
		// disagree with what was actually configured.
		if (View.Mesh != nullptr)
		{
			View.AnimClass = Content->VehicleAnimClass.LoadSynchronous();
		}
	}
	return View;
}
