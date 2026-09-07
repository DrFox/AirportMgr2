#include "Content/AirsideSettings.h"

#include "Content/AirsideContent.h"
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
	Van.Ground.MaxTurnRateDegPerSec = 90.0;

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
