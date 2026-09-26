#include "Content/AirsideSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Content/AirsideContent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialParameterCollection.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"

// ONE CONSTANT PER ARTICULATED VEHICLE, for FVehicle::TypeCode - what the inspector, and the
// dispatch log (AirsideTraffic.cpp), say this vehicle is. NAMED, not anonymous, because the
// module is a unity build.
//
// NO LONGER A LOOK-UP KEY (#308). UAirsideSettings::ResolveVehicleViewFor used to branch on
// this to choose ResolveRigView/ResolveUtilityTowView - a TypeCode ladder that grew by one
// branch per vehicle added to dispatch. It now loads FVehicle::Mesh directly, which
// ResolveRigVehicle/ResolveUtilityTowVehicle below fill themselves; these two constants are
// left purely for the inspector's and the log's sake.
// ENFORCED BY: Check-Architecture's no-vehiclecode-compare rule (AirsideVehicleCodes:: banned
// on either side of ==/!= anywhere - the ladder shape, not merely this one former call site).
namespace AirsideVehicleCodes
{
	static const TCHAR* const Rig = TEXT("RIG");
	static const TCHAR* const UtilityTow = TEXT("UTILITY");
}

// File-local, matching every other category in this module.
DEFINE_LOG_CATEGORY_STATIC(LogAirsideContent, Log, All);

UAirsideSettings::UAirsideSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Airside");
}

int32 UAirsideSettings::GetContentCallCountForTest = 0;

const UAirsideContent* UAirsideSettings::GetContent()
{
	++GetContentCallCountForTest;

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
	Piper.Chassis.Ground = UAircraftType::PiperMeridianGround();
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

int32 UAirsideSettings::ResolveLargestServiceVehicleCallCountForTest = 0;

FChassis UAirsideSettings::ResolveLargestServiceVehicle()
{
	// COUNTED BEFORE ANYTHING ELSE - see the counter's own comment. Issue #190: this used to
	// be called fresh per arm, per ordered arm pair and twice per link; a production caller
	// now resolves it ONCE per rebuild and passes the answer down instead of calling back in.
	++ResolveLargestServiceVehicleCallCountForTest;

	// ONE CLASS TODAY, and deliberately no taxonomy yet: an EVehicleClass enum with a single
	// member would be a list nothing chooses from - the "authored numbers nothing reads"
	// failure this codebase has shipped three times. When the second dispenser arrives, this
	// function gains the comparison and every service road corner widens on the next rebuild,
	// with no other site to find.
	return ResolveDefaultVehicle().Chassis;
}

FVehicle UAirsideSettings::ResolveLargestServiceBody()
{
	FVehicle Out = ResolveDefaultVehicle();
	Out.Chassis = ResolveLargestServiceVehicle();
	return Out;
}

namespace AirsideSettingsTierCache
{
	/** What ResolveTierDesignVehicles last resolved, and from which content set and Wide asset. */
	struct FCache
	{
		bool bValid = false;
		TWeakObjectPtr<const UAirsideContent> Content;
		FSoftObjectPath WidePath;
		TMap<TObjectKey<URoadProfile>, FVehicle> Map;
	};

	FCache& Get()
	{
		static FCache Cache;
		return Cache;
	}
}

int32 UAirsideSettings::ResolveTierDesignVehiclesCallCountForTest = 0;

void UAirsideSettings::ResetTierDesignVehiclesCacheForTest()
{
	AirsideSettingsTierCache::Get() = AirsideSettingsTierCache::FCache();
	ResolveTierDesignVehiclesCallCountForTest = 0;
}

const TMap<TObjectKey<URoadProfile>, FVehicle>& UAirsideSettings::ResolveTierDesignVehicles()
{
	AirsideSettingsTierCache::FCache& Cache = AirsideSettingsTierCache::Get();

	// THE CHEAP CHECK: the content set already in memory (Get, never a load) and the path its
	// Wide tier names. Either changing - another set configured, the tier list re-authored -
	// is a miss; nothing else in the answer can change while the editor runs (the rig's figures
	// are code).
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	const UAirsideContent* InMemory = Settings != nullptr ? Settings->Content.Get() : nullptr;
	if (Cache.bValid && InMemory != nullptr && Cache.Content.Get() == InMemory)
	{
		const bool bHasWide = InMemory->ServiceRoadProfiles.IsValidIndex(WideServiceTier);
		const FSoftObjectPath WidePath = bHasWide ? InMemory->ServiceRoadProfiles[WideServiceTier].ToSoftObjectPath() : FSoftObjectPath();
		if (WidePath == Cache.WidePath)
		{
			return Cache.Map;
		}
	}

	++ResolveTierDesignVehiclesCallCountForTest;
	Cache = AirsideSettingsTierCache::FCache();
	const UAirsideContent* Content = GetContent();
	// NO WIDE TIER, NO EXCEPTION: a content set with fewer tiers than Wide's index has nothing
	// designed for the rig, and every road stays sized for the default - never the narrowest
	// tier promoted to the rig's, which would oversize the one road a small set lays.
	if (Content != nullptr && Content->ServiceRoadProfiles.IsValidIndex(WideServiceTier))
	{
		Cache.WidePath = Content->ServiceRoadProfiles[WideServiceTier].ToSoftObjectPath();
		if (const URoadProfile* Wide = Content->ServiceRoadProfiles[WideServiceTier].LoadSynchronous())
		{
			// THE RIG ON WIDE (user ruling 2026-09-25): its lock radius, 370 / sin 40 = 576 uu
			// against the bowser's 510, is what the Wide tier's corners are laid for.
			// The WHOLE rig since 2026-09-25: its trailer is what a Wide bend's inside is widened for.
			Cache.Map.Add(TObjectKey<URoadProfile>(Wide), ResolveRigVehicle());
		}
	}
	Cache.Content = Content;
	Cache.bValid = Content != nullptr;
	return Cache.Map;
}

FRoadDesignVehicles UAirsideSettings::ResolveRoadDesignVehicles()
{
	FRoadDesignVehicles Out(ResolveLargestServiceBody());
	Out.PerProfile = ResolveTierDesignVehicles();
	return Out;
}

FVehicle UAirsideSettings::ResolveRigVehicle()
{
	// The DEFAULT VEHICLE'S PERFORMANCE, a rigid truck's, with the rig's own geometry: nothing
	// here has measured how a loaded articulated tanker accelerates, and inventing figures
	// would be worse than inheriting honest ones. Geometry is what gating needs.
	FVehicle Rig = ResolveDefaultVehicle();
	Rig.TypeCode = AirsideVehicleCodes::Rig;

	// MEASURED from truckCab1.glb and tankTrailer1.glb on 2026-09-24. Tractor: steer_FL/FR at
	// x 370 from the rear-axle origin; body 516 ahead, 78 behind; 254 over the body (mirrors
	// excluded); fifth wheel 57.3 ahead of the rear axle. Trailer: kingpin 1029.5 ahead of the
	// tandem centre; body 166 ahead of the kingpin and 121.5 behind the tandem; 254 wide.
	Rig.Chassis.SteerAxleX = 370.0;
	Rig.Chassis.FixedAxleX = 0.0;
	Rig.BodyWidth = 254.0;
	Rig.BodyFrontX = 516.0;
	Rig.BodyRearX = -78.0;
	// ONE LINK: a semi-trailer is the one-link tow (see FTowLink). Figures unchanged from when
	// this was FTrailer, so route gating (#276) judges the rig exactly as it did.
	FTowLink Trailer;
	Trailer.HitchX = 57.3;
	Trailer.Length = 1029.5;
	Trailer.BodyFront = 166.0;
	Trailer.BodyRear = 121.5;
	Trailer.Width = 254.0;
	Rig.Tow = { Trailer };

	// THE LOOK, READ HERE RATHER THAN BY A SEPARATE ResolveRigView, since #308: the rig has no
	// UVehicleType asset yet (#287), so the Mesh/AnimClass fields a real asset's Vehicle() would
	// fill are named here instead, from UAirsideContent's RigCabMesh/RigCabAnimClass/
	// RigTrailerMesh/RigTrailerAnimClass - the ONE place those four fields are read now that
	// ResolveRigView only forwards to ResolveVehicleViewFor(ResolveRigVehicle()). RETIRE this
	// block, and the four content fields, the day #287 gives the rig a DA_Vehicle_Rig1 asset
	// whose own Vehicle() names them instead. Dated 2026-09-25.
	// ENFORCED BY: Check-Architecture's RigUtilityLookContentFields allowed-callers row (a
	// second reader of these four fields outside this file fails).
	if (const UAirsideContent* Content = GetContent())
	{
		Rig.Mesh = Content->RigCabMesh;
		Rig.AnimClass = Content->RigCabAnimClass;
		Rig.Tow[0].Mesh = Content->RigTrailerMesh;
		Rig.Tow[0].AnimClass = Content->RigTrailerAnimClass;
	}

	// 40 degrees is ASSUMED - the model carries no lock, and 40-45 is a tractor unit's range.
	// The lock allows 5.8 m at the steered axle. In a STEADY circle the trailer folds below
	// ~10.9 m, but a real 90 degree corner ends before it settles: VehicleSweep::Trace, which
	// route search uses, gets it round a 5.7 m corner without jack-knifing.
	//
	// THE TRAILER IS LONGER THAN A ROAD-LEGAL ONE: 10.3 m kingpin to axles, where the EU
	// turning circle (12.5 m / 5.3 m) needs about 7.7 m with this cab - see
	// Airside.Solve.VehicleSweepEuCircle, which reports it. A model question, raised 2026-09-24.
	Rig.Chassis.Ground.MaxSteerDegrees = 40.0;
	return Rig;
}

FVehicle UAirsideSettings::ResolveDefaultVehicle()
{
	// NO CONTENT LOOKUP, unlike ResolveDefaultAirframe above, and deliberately: there is no
	// UVehicleType asset to point at yet, so a soft pointer here would be a content slot
	// nothing could fill - the "authored numbers nothing reads" failure, one level up. M3
	// adds the type with the fleet, and this function is where it will be resolved.
	FVehicle Van;

	// A LIGHT COMMERCIAL VEHICLE, in FGroundRegime's units (uu/s and uu/s^2; a uu is a
	// centimetre). 1 m/s^2 up, 2 m/s^2 braking, 10 m/s flat out - an airside speed limit
	// rather than a road one. Written out rather than left to the struct defaults because
	// this is where a truck's performance is DECIDED, and a reader must be able to see the
	// figures without opening another header to find out they happen to coincide.
	Van.Chassis.Ground.Taxi.Accel = 100.0;
	Van.Chassis.Ground.Taxi.Decel = 200.0;
	Van.Chassis.Ground.Taxi.SpeedCap = 1000.0;

	// ZERO, AND IT MEANS IT. A truck's wheels are driven and steered independently of any
	// thrust line, so it can stop with the wheel turned and pull away again. None of the "a
	// wheeled aircraft cannot yaw without rolling" physics MinSteeringSpeed exists for applies
	// to it.
	//
	// It was 50 uu/s until 2026-09-15, justified as "a follower allowed to stop dead mid-turn
	// would snap its heading round". True of the PIVOT law, and it stopped being true the
	// moment this truck got measured axles: under rolling-steer the yaw is v*sin(d)/L, so at
	// zero speed the heading cannot move at all, let alone snap. What genuinely needs a
	// non-zero floor is the SOLVER, not the vehicle - FRouteFollower::ProgressEpsilon.
	Van.Chassis.Ground.MinSteeringSpeed = 0.0;

	// 0.3 g. AUTHORED RATHER THAN INHERITED, which it was until 2026-09-15: the struct default
	// is 147 uu/s^2, an AIRCRAFT CABIN comfort figure, and nothing here ever chose it. A van on
	// dry concrete does 0.3 g without drama, and this is what decides corner speed once
	// steering is geometric - at the 510 uu the lock allows, the difference between 9.9 km/h
	// and 13.9 (699 uu, 11.5 and 16, while the truck was 8.5 m). The note above about writing every figure out rather than inheriting it applies
	// to this one too; it was simply missed.
	Van.Chassis.Ground.MaxLateralAccelUu = 294.0;

	// NINE TIMES an airframe's 10 deg/s. A van turns into a depot in its own length; an
	// aircraft's rate here would sweep it across the kerb and back.
	//
	// A CEILING NOW, NOT THE LAW, because the axles below turn the geometric law on. It stays
	// as the guard it always was against a rate nothing else bounds.
	Van.Chassis.Ground.MaxTurnRateDegPerSec = 90.0;

	// MEASURED AXLES, which is what stops the truck PIVOTING.
	//
	// FChassis::Wheelbase's own comment argues the other way - "a van is authored at 90
	// deg/s and would need 83 degrees of lock at its creep speed, so it is pivoting rather
	// than steering, and a bicycle model would cripple every service vehicle on the airport".
	// That was right about a van NOBODY HAD MEASURED: with no axles the wheelbase is zero,
	// HasAxles() is false, and the flat rate is all there is. Its arithmetic is also exactly
	// the symptom - 90 deg/s at the 50 uu/s creep is a 0.32 m radius, which is not a turn,
	// it is a spin about the rear axle. Reported from play on 2026-09-14: the truck drives up
	// to the stand, stops, swings 90 degrees on the spot and drives off.
	//
	// fueltruck1 HAS been measured. Its rig puts steer_FL/FR at x = 355.0 uu and wheel_RL/RR
	// at the origin, so the wheelbase is 3.55 m and the geometric law has real figures to
	// work with.
	//
	// 3.55 m SINCE 2026-09-25, THE rigidCab1 CHASSIS. The Tripo truck was replaced by a bowser
	// body on rigidCab1 (a DAF LF class 4x2, wheelbase from its body-builder drawing NSEA606),
	// built in rigidCab1.blend and reimported over SK_FuelTruck1. 5.7 cm shorter between the
	// axles than the 3.607 below; DA_Vehicle_FuelTruck1 states the same figures and
	// AirportMgr.Content.VehicleTypes.FuelTruckAgreesWithDispatch holds the two together.
	//
	// (History) BACK TO 3.607 m ON 2026-09-24, with the truck back to its modelled 6.2 m. The 8.5 m
	// enlargement below was made to test crabbing, and uniform scaling took the width to
	// 3.26 m over the tyres - wider than the 3 m road lane it drives in once roads had lanes
	// and vehicles were gated on width. Shrunk as it was grown, uniformly, in the model.
	//
	// (History) RESIZED 2026-09-15 from 3.607 m, when the truck itself went from 6.2 m to 8.5. The old
	// figures were a Ford Transit's: an 11.2 m kerb-to-kerb circle, parked beside a 737. 8.5 m
	// is the SMALLEST real hydrant dispenser, and larger classes follow - which is why the
	// road this turns on is sized from ResolveLargestServiceVehicle and never from here.
	//
	// The resize happened in the MODEL, not at import: align_and_scale.py's LENGTH_TARGET.
	// Interchange's import_offset_uniform_scale was tried first and cannot do it - it lands
	// twice on a skinned mesh's vertices and once on its bind pose, so it produced a correct
	// 8.5 m body on a 4.22 m wheelbase. Airside.Content.VehicleFootprintMatchesTheMesh caught
	// it, and the rejection is written down in Tools/Python/import_fueltruck.py.
	//
	// ORIGIN AT THE FIXED AXLE, which is the Piper's declared deviation rather than plane2's
	// nose-gear convention - and it is the model's, not a choice made here: fueltruck1's
	// README fixes its origin at "rear axle centre, projected to the ground, because that is
	// what a front-steered truck pivots about". Airside.Content.VehicleFootprintMatchesTheMesh
	// asserts both figures against the mesh's own bones so they cannot drift from it.
	// DECLARED, since the law stopped being inferred from these two numbers on
	// 2026-09-15 - see ESteerLaw. A truck steers on a front axle; it does not pivot.
	Van.Chassis.SteerLaw = ESteerLaw::RollingSteer;
	Van.Chassis.SteerAxleX = 355.0;
	Van.Chassis.FixedAxleX = 0.0;

	// 45 degrees, written out rather than left at FGroundRegime's 60. The struct default is
	// an aircraft nose gear's, and a rigid truck does not have that: 60 would give a 4.2 m
	// radius on this wheelbase, a turning circle no rigid truck of this size makes.
	//
	// BACK TO 45 FROM 50, 2026-09-15, AND THE 50 WAS NEVER A MEASUREMENT. It was raised on
	// 2026-09-14 so the truck's lock would clear a service road fillet authored at 500 uu -
	// which is shrinking a vehicle's turning circle to fit the road, the exact inverse of the
	// rule this airport uses everywhere else: size ground geometry for the largest thing
	// admitted, never for the one using it now. The fillet is DERIVED from the vehicle now
	// (URoadProfile::ResolvedFilletRadius), so the lock can go back to what a rigid truck has.
	//
	// 45 on the 3.55 m wheelbase is a 5.02 m front-axle radius (5.10 m at 3.607, 6.99 m at
	// 8.5 m). Reversing is tighter still, Wheelbase / tan(lock) = 3.55 m, which is why a driver
	// backs into a tight space rather than nosing in.
	Van.Chassis.Ground.MaxSteerDegrees = 45.0;

	// THE BODY, measured from rigidCab1/export/fueltruck1.glb on 2026-09-25 by
	// build_vehicle_types.py: 482.5 ahead of the rear axle to the front bumper, 187.0 behind it
	// to the frame end, 226.0 over the body with the wing mirrors excluded (2.80 m with them).
	// 669.5 overall, which is VehicleFootprint - see Airside.Model.VehicleBody, which holds the
	// two together. (It was 481.8 / 138.2 / 237.4 on the 6.2 m Tripo truck: the new chassis is
	// 11 mm longer at the front, 49 cm longer behind, and 11 cm narrower.)
	Van.BodyWidth = 226.0;
	Van.BodyFrontX = 482.5;
	Van.BodyRearX = -187.0;

	// A TRUCK CANNOT FLY, AND NOW CANNOT EVEN BE ASKED TO. Until 2026-09-23 this was an
	// FAirframe, and its climb and approach were zeroed here by hand because their struct
	// defaults are a light twin's and all NON-ZERO - a default FApproachPerformance answers
	// IsSet() == true, so an unzeroed van would have passed as landable to ArrivalPlanner and
	// FRoadAgent, which both branch on exactly that call. An FVehicle has no climb, approach,
	// gear or engine to zero, and FRoadAgent::AsAircraft() answers null for it, so the
	// question is unrepresentable rather than answered false.
	//
	// THE ENGINE WENT WITH THEM. It was left at its defaults so FRoadAgent would spool a
	// propeller RPM into FAgentMotion for a truck; the one reader, UAirsideAgentAnim, drives a
	// prop, and a truck's RPM is now zero - see FRoadAgent::StartEngineAtSpeed.
	//
	// What the inspector SAYS this is - see FVehicle::TypeCode. FUEL rather than VAN
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

UEntityDefinition* UAirsideSettings::ResolvePlaceable(EPlaceableEntity Kind)
{
	const UAirsideContent* Content = GetContent();
	if (Content == nullptr)
	{
		return nullptr;
	}
	// FIND, NOT [] - an unmapped kind is a supported state (no content set has authored one
	// yet), the same as every other Resolve* in this file, and TMap::operator[] asserts on a
	// missing key rather than answering null.
	const TSoftObjectPtr<UEntityDefinition>* Found = Content->Placeables.Find(Kind);
	return Found != nullptr ? Found->LoadSynchronous() : nullptr;
}

UAircraftType* UAirsideSettings::ResolveLargestAircraftOfLetter(EIcaoCode Letter)
{
	// SEE THE HEADER: no scannable per-letter list exists (UAirsideContent carries one
	// DefaultAircraft soft pointer, not a table), and adding an AssetRegistry scan for one
	// caller would be a second table by another name. Null for every letter until a real
	// one is authored and this function is the one place that starts resolving it.
	//
	// #292 DID ADD AN ASSETREGISTRY SCAN TO THIS FILE (see ResolveLetterEnvelope below), but
	// it answers a different question - "what are Letter's FIGURES", not "which ASSET is the
	// biggest" - and never needs to hand back a UAircraftType* the caller could dereference.
	// A future caller of THIS function still wants the actual asset (to draw it parked, say),
	// which is the harder problem this function's header names; ResolveLetterEnvelope solves
	// the narrower one.
	(void)Letter;   // kept named for the caller and for when a real table lands
	return nullptr;
}

int32 UAirsideSettings::ResolveLetterEnvelopeCallCountForTest = 0;

namespace
{
	/** ResolveLetterEnvelope/Table's cache - every letter, resolved together since one
	 *  AssetRegistry scan answers all six at once. See ResolveLetterEnvelope's own comment
	 *  on why this is a plain lazy-once cache rather than ResolveTierDesignVehicles' own
	 *  content-pointer invalidation: there is no single UAirsideContent driving this scan for
	 *  that pattern to key off. */
	struct FLetterEnvelopeCache
	{
		bool bValid = false;
		FLetterEnvelopeTable Table;
	};

	FLetterEnvelopeCache& GetLetterEnvelopeCache()
	{
		static FLetterEnvelopeCache Cache;
		return Cache;
	}
}

void UAirsideSettings::ResetLetterEnvelopeCacheForTest()
{
	GetLetterEnvelopeCache() = FLetterEnvelopeCache();
	ResolveLetterEnvelopeCallCountForTest = 0;
}

FLetterEnvelope UAirsideSettings::EnvelopeFromFleet(EIcaoCode Letter, TArrayView<UAircraftType* const> Fleet)
{
	// THE AUTHORED ROW IS THE FLOOR, NOT A DIFFERENT SEED - a letter with no modelled type, or
	// an asset that fails to load or parse, keeps EXACTLY today's numbers rather than 0, which
	// every caller through StandBox would otherwise read as "no clearance at all".
	FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);

	for (UAircraftType* Type : Fleet)
	{
		if (Type == nullptr)
		{
			continue;
		}

		// PARSED, NOT TRUSTED - Code is an EditAnywhere FName a human can mistype, the same
		// reason Airside.Content.MeasuredTypesFitTheirLettersRow parses rather than compares
		// strings. A type of a DIFFERENT letter, or one that fails to parse at all, does not
		// touch this letter's envelope.
		const TOptional<EIcaoCode> Parsed = IcaoCode::Parse(Type->Code.ToString());
		if (!Parsed.IsSet() || *Parsed != Letter)
		{
			continue;
		}

		// NOSE-GEAR COORDINATES, the same conversion Airside.Content.
		// MeasuredTypesFitTheirLettersRow and Airside.Entities.EveryAirframeFitsItsLettersRow
		// both apply before comparing a footprint against this same row.
		const double ToStopMark = Type->SteerAxleX;
		const double Nose = Type->Footprint.NoseX - ToStopMark;
		const double Tail = Type->Footprint.TailX - ToStopMark;

		Envelope.MaxTailAft = FMath::Max(Envelope.MaxTailAft, -Tail);
		Envelope.MaxNoseFwd = FMath::Max(Envelope.MaxNoseFwd, Nose);
	}

	return Envelope;
}

const FLetterEnvelopeTable& UAirsideSettings::ResolveLetterEnvelopeTable()
{
	FLetterEnvelopeCache& Cache = GetLetterEnvelopeCache();
	if (!Cache.bValid)
	{
		// COUNTED BEFORE ANYTHING ELSE - ResolveLargestServiceVehicleCallCountForTest's own
		// reason: this is the one place a production caller should ever pay for the scan.
		++ResolveLetterEnvelopeCallCountForTest;

		// EVERY LOADED UAircraftType, ONCE, via the AssetRegistry - see ResolveLargestAircraftOfLetter's
		// own header for why no in-content table exists to walk instead. bSearchSubClasses
		// false: a UAircraftType subclass would be a second kind of type nothing else here
		// treats specially, and this scan should not either without that being decided first.
		TArray<FAssetData> Assets;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();
		// SYNCHRONOUS, because a scan that returns before the registry has finished its own
		// startup discovery would silently under-count the fleet - a letter would read as
		// having nothing modelled purely from being asked too early. A no-op once the registry
		// is already caught up (IsSearchAllAssets() true, which it is by the time PIE or a
		// test runs in the ordinary case).
		Registry.SearchAllAssets(true);
		Registry.GetAssetsByClass(UAircraftType::StaticClass()->GetClassPathName(), Assets, false);

		TArray<UAircraftType*> Fleet;
		Fleet.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			if (UAircraftType* Type = Cast<UAircraftType>(Asset.GetAsset()))
			{
				Fleet.Add(Type);
			}
		}

		for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Cache.Table.Envelopes); ++Index)
		{
			Cache.Table.Envelopes[Index] = EnvelopeFromFleet(static_cast<EIcaoCode>(Index), Fleet);
		}
		Cache.bValid = true;
	}
	return Cache.Table;
}

const FLetterEnvelope& UAirsideSettings::ResolveLetterEnvelope(EIcaoCode Letter)
{
	return ResolveLetterEnvelopeTable()[Letter];
}

UStaticMesh* UAirsideSettings::ResolveVehicleMesh()
{
	const UAirsideContent* Content = GetContent();
	return Content != nullptr ? Content->VehicleMesh.LoadSynchronous() : nullptr;
}

FFenceKit UAirsideSettings::ResolveFenceKit()
{
	FFenceKit Kit;
	const UAirsideContent* Content = GetContent();
	if (Content != nullptr)
	{
		Kit.LinePost = Content->FenceLinePost.LoadSynchronous();
		Kit.HeavyPost = Content->FenceHeavyPost.LoadSynchronous();
		Kit.Fabric = Content->FenceFabricMaterial.LoadSynchronous();

		// THE NAME IS build_fence_content.py's FADE list's. A missing parameter leaves 0 - no
		// cull - rather than culling posts the material is still drawing.
		// ENFORCED BY: Airside.Content.FenceFadeWired (the collection carries PostFadeEnd)
		if (const UMaterialParameterCollection* Fade = Content->FenceFadeCollection.LoadSynchronous())
		{
			bool bFound = false;
			const float End = Fade->GetScalarParameterDefaultValue(TEXT("PostFadeEnd"), bFound);
			Kit.PostFadeEndUu = bFound ? End : 0.0;
		}
	}
	return Kit;
}

TArray<FDepotModuleLook> UAirsideSettings::ResolveDepotLooks()
{
	TArray<FDepotModuleLook> Looks;
	Looks.SetNum(static_cast<int32>(EDepotModule::Count));

	const UAirsideContent* Content = GetContent();
	if (Content == nullptr)
	{
		return Looks;
	}

	for (int32 Raw = 0; Raw < Looks.Num(); ++Raw)
	{
		const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(static_cast<EDepotModule>(Raw));
		const UPlotModuleKit* Kit = Found != nullptr ? Found->Get() : nullptr;
		if (Kit == nullptr)
		{
			continue;
		}

		FDepotModuleLook& Look = Looks[Raw];
		Look.Assembly = Kit->Assembly;
		// A QUARTER TURN, NEVER BETWEEN: the presenter lays pieces out by their bounds, and only
		// a quarter turn keeps a mesh's bounds a box in the kit's frame. 45 would widen it.
		Look.MeshYawDeg = FMath::RoundToDouble(Kit->MeshYawDeg / 90.0) * 90.0;
		if (Kit->Assembly == EKitAssembly::Parts)
		{
			Look.Cap = Kit->PartCapMesh.LoadSynchronous();
			Look.Bay = Kit->PartBayMesh.LoadSynchronous();
		}
		else
		{
			for (const TSoftObjectPtr<UStaticMesh>& Mesh : Kit->BakedMeshes)
			{
				// A NULL ENTRY ENDS THE LIST rather than leaving a hole: index is bay count - 1,
				// so a hole would put a two-bay mesh under a one-bay run.
				UStaticMesh* Loaded = Mesh.LoadSynchronous();
				if (Loaded == nullptr)
				{
					break;
				}
				Look.Baked.Add(Loaded);
			}
		}
	}
	return Looks;
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

FResolvedTowView UAirsideSettings::ResolveRigView()
{
	// A THIN WRAPPER SINCE #308. ResolveRigVehicle now names its own Mesh/AnimClass/Tow[].Mesh
	// (see its own comment for where from, for now), so resolving them is
	// ResolveVehicleViewFor's job like any other vehicle's. Kept as its own named function
	// because RigActorTest and the content tests ask for "the rig's look" without wanting to
	// reassemble ResolveRigVehicle() themselves.
	return ResolveVehicleViewFor(ResolveRigVehicle());
}

FVehicle UAirsideSettings::ResolveUtilityTowVehicle()
{
	// THE DEFAULT VEHICLE'S PERFORMANCE, for the reason ResolveRigVehicle gives: nothing here
	// has measured how a loaded bowser trailer accelerates behind a baggage tug, and inventing
	// figures would be worse than inheriting honest ones. Geometry is what gating needs.
	FVehicle Utility = ResolveDefaultVehicle();
	Utility.TypeCode = AirsideVehicleCodes::UtilityTow;

	// utility1 (the TUG MA-50, utility1/SPEC.md): steer_FL/FR at X 149.3 from the rear-axle
	// origin, MEASURED off the imported SK_Utility1's reference pose (Tools/Python/
	// import_fueltrailer1.py's report_tow_chain, 2026-09-24) - 1.493 m, matching utility1/
	// README.md's own verification table ("wheelbase 1.493", 0.4% short of SPEC.md's 1.499
	// target, "measured, not aimed at"). The BODY - 208.6 ahead of the rear axle, 95.7 behind
	// it (the coupler reaches behind the axle - see the hitch figure below), 172.6 wide (over
	// the tyres, mirrors excluded) - is SK_Utility1's own mesh bounds, same measurement.
	Utility.Chassis.SteerAxleX = 149.3;
	Utility.Chassis.FixedAxleX = 0.0;
	Utility.BodyWidth = 172.6;
	Utility.BodyFrontX = 208.6;
	Utility.BodyRearX = -95.7;

	// THE TOW IS A DRAWBAR CHAIN, TWO LINKS (spec 2026-09-24 revision, section 4): a towbar
	// (BAR - both BodyFront and BodyRear zero, per FTowLink's own comment), then the body.
	// Both links MEASURED off the imported SK_Utility1 and SK_FuelTrailer1 on 2026-09-24 -
	// Tools/Python/import_fueltrailer1.py's report_tow_chain, which prints every figure below
	// with its own derivation at the site it measured it.
	//
	// LINK 0, THE TOWBAR. HitchX: utility1's own 'hitch' bone, X=-90.68 uu - README.md's own
	// "hitch socket... 0.907 m behind utility1's rear axle" (added 2026-09-23), matching to a
	// centimetre. Length: the towbar's own hitch (its 'tow_eye' bone, X=335.0) to its own axle
	// ('towbar_yaw', X=221.0) - 114.0 uu, matching README's "eye 1.14 from the yaw axis".
	// Width: the combined X/Y bounds of the towbar-region mesh parts (axle_front, yoke,
	// towbar, both knuckles, both front wheels) - 128.0 uu, narrower than the body since it is
	// what actually sweeps between the hitch and the axle (FTowLink's own comment).
	FTowLink Towbar;
	Towbar.HitchX = -90.7;
	Towbar.Length = 114.0;
	Towbar.BodyFront = 0.0;
	Towbar.BodyRear = 0.0;
	Towbar.Width = 128.0;

	// LINK 1, THE BODY. HitchX 0.0: the body's hitch IS the towbar's own axle (towbar_yaw) -
	// there is no further offset between where the towbar ends and the body's turntable
	// begins, so unlike the towbar's HitchX this one is exact by construction, not measured.
	// Length: towbar_yaw (the hitch) to the rear axle ('root', the origin) - 221.0 uu, README's
	// own "wheelbase 2.21" (baggageCart1's rig, number for number) x 100. BodyFront/BodyRear:
	// the combined bounds of the body-region mesh parts (frame, tank and its fittings, the
	// control bay, hose reel, nozzle, rear lamps/clevis, mudguards, the rear axle, both rear
	// wheels) against towbar_yaw and the origin - 48.0 uu ahead of the hitch (the control bay
	// oversails the turntable), 44.1 uu behind the axle. Width 146.4 uu matches README's own
	// "overall... 1.46 wide" exactly.
	FTowLink Body;
	Body.HitchX = 0.0;
	Body.Length = 221.0;
	Body.BodyFront = 48.0;
	Body.BodyRear = 44.1;
	Body.Width = 146.4;

	Utility.Tow = { Towbar, Body };

	// THE LOOK, FOR THE SAME REASON ResolveRigVehicle's own comment gives (#308/#287): utility1
	// and fuelTrailer1 have no UVehicleType asset yet, so their Mesh/AnimClass are named here
	// from UAirsideContent's UtilityMesh/UtilityAnimClass/UtilityTrailerMesh/
	// UtilityTrailerAnimClass - the ONE place those four fields are read now that
	// ResolveUtilityTowView only forwards to ResolveVehicleViewFor(ResolveUtilityTowVehicle()).
	// Tow[0], the towbar, is left unnamed: fuelTrailer1 is one skinned asset covering the whole
	// body (see UtilityTrailerMesh's own comment), so there is nothing for a second mesh to
	// name. RETIRE this block, and the four content fields, when #287 gives utility1 a
	// DA_Vehicle_UtilityTow1 asset whose own Vehicle() names them instead. Dated 2026-09-25.
	// ENFORCED BY: Check-Architecture's RigUtilityLookContentFields allowed-callers row (a
	// second reader of these four fields outside this file fails).
	if (const UAirsideContent* Content = GetContent())
	{
		Utility.Mesh = Content->UtilityMesh;
		Utility.AnimClass = Content->UtilityAnimClass;
		Utility.Tow[1].Mesh = Content->UtilityTrailerMesh;
		Utility.Tow[1].AnimClass = Content->UtilityTrailerAnimClass;
	}

	// THE LOCK IS LEFT AT ResolveDefaultVehicle's 45 degrees, UNMEASURED for utility1 itself -
	// unlike the rig's 40, nothing here has reason to override it: this task's own tests gate
	// on the CHAIN's geometry (link count, link lengths against the skeleton), not on how
	// tightly utility1 can turn. utility1/SPEC.md's own "turning radius 115 in (2.921 m)"
	// would resolve to about 27 degrees on this wheelbase (atan(1.493/2.921)) if measured
	// properly against the model rather than the datasheet, and is a follow-up, not this one.
	return Utility;
}

FResolvedTowView UAirsideSettings::ResolveUtilityTowView()
{
	// SEE ResolveRigView's OWN COMMENT - the same wrapper, for the same reason: fuelTrailer1's
	// Tow[0] (the towbar) resolves to an empty FResolvedAgentView here exactly as it always did,
	// because ResolveUtilityTowVehicle never names a Mesh for it (see its own comment) and
	// ResolveVehicleViewFor's per-link loop leaves an unnamed link's Mesh null.
	return ResolveVehicleViewFor(ResolveUtilityTowVehicle());
}

FResolvedTowView UAirsideSettings::ResolveVehicleViewFor(const FVehicle& Vehicle)
{
	// LOAD WHAT THE VEHICLE NAMES, ELSE THE GAME-WIDE DEFAULT - ResolveAgentView's own shape,
	// one level down. NO CODE BRANCH ANY MORE (#308): this used to be a TypeCode ladder
	// ("if TypeCode == RIG, return ResolveRigView()...") that a THIRD site - beside the
	// Resolve*Vehicle stamping the code and the content fields the ladder's target read - had
	// to agree with. A vehicle now carries its own look (FVehicle::Mesh/AnimClass, and per Tow
	// link), filled by UVehicleType::Vehicle() from an asset or by ResolveRigVehicle/
	// ResolveUtilityTowVehicle from content for now, so this function does not need to know
	// which vehicle it was handed, the same as ResolveAgentView does not need to know which
	// aircraft.
	FResolvedTowView View;
	View.Cab.Mesh = Vehicle.Mesh.LoadSynchronous();
	if (View.Cab.Mesh != nullptr)
	{
		View.Cab.AnimClass = Vehicle.AnimClass.LoadSynchronous();
	}
	else
	{
		// NOTHING NAMED: the one vehicle look there has always been - so a vehicle that somehow
		// carried a Tow with no look of its own shows its cab alone, and SpawnView's count check
		// says so.
		View.Cab = ResolveVehicleView();
	}

	// ONE ENTRY PER TOW LINK, IN ORDER - FResolvedTowView's own contract. A link with no Mesh
	// named (a bar, or one nobody has authored yet) resolves to an empty FResolvedAgentView
	// rather than being left out of the array, so Links[i] always answers Tow[i].
	View.Links.SetNum(Vehicle.Tow.Num());
	for (int32 Link = 0; Link < Vehicle.Tow.Num(); ++Link)
	{
		View.Links[Link].Mesh = Vehicle.Tow[Link].Mesh.LoadSynchronous();
		if (View.Links[Link].Mesh != nullptr)
		{
			View.Links[Link].AnimClass = Vehicle.Tow[Link].AnimClass.LoadSynchronous();
		}
	}
	return View;
}
