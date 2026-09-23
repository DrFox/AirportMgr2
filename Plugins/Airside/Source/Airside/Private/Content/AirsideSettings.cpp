#include "Content/AirsideSettings.h"

#include "Content/AirsideContent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialParameterCollection.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"

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
	// steering is geometric - at the 699 uu the lock allows, the difference between 11.5 km/h
	// and 16. The note above about writing every figure out rather than inheriting it applies
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
	// fueltruck1 HAS been measured. Its rig puts steer_FL/FR at x = 494.5 uu and wheel_RL/RR
	// at the origin, so the wheelbase is 4.945 m and the geometric law has real figures to
	// work with.
	//
	// RESIZED 2026-09-15 from 3.607 m, when the truck itself went from 6.2 m to 8.5. The old
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
	Van.Chassis.SteerAxleX = 494.5;
	Van.Chassis.FixedAxleX = 0.0;

	// 45 degrees, written out rather than left at FGroundRegime's 60. The struct default is
	// an aircraft nose gear's, and a rigid truck does not have that: 60 would give a 5.7 m
	// radius on this wheelbase, a turning circle no 8.5 m truck makes.
	//
	// BACK TO 45 FROM 50, 2026-09-15, AND THE 50 WAS NEVER A MEASUREMENT. It was raised on
	// 2026-09-14 so the truck's lock would clear a service road fillet authored at 500 uu -
	// which is shrinking a vehicle's turning circle to fit the road, the exact inverse of the
	// rule this airport uses everywhere else: size ground geometry for the largest thing
	// admitted, never for the one using it now. The fillet is DERIVED from the vehicle now
	// (URoadProfile::ResolvedFilletRadius), so the lock can go back to what a rigid truck has.
	//
	// 45 on a 4.945 m wheelbase is a 6.99 m front-axle radius - a kerb-to-kerb circle near
	// 16.2 m, correct for a rigid 8.5 m truck, where the old figures gave 11.2 m and a
	// Transit. Reversing is tighter still, Wheelbase / tan(lock) = 4.95 m, which is why a
	// driver backs into a tight space rather than nosing in.
	Van.Chassis.Ground.MaxSteerDegrees = 45.0;

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
