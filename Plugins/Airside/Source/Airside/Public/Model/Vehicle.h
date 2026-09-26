#pragma once

// A SERVICE VEHICLE'S BUNDLE, split out on 2026-09-23. Until then a fuel truck was an
// FAirframe with its climb and approach zeroed by hand (UAirsideSettings::ResolveDefaultVehicle
// called that scaffolding, "M3 replaces this with a vehicle-shaped performance bundle"), so a
// truck carried a gear-retraction cycle, a propeller spool and a wingspan it had no use for,
// and every consumer had to remember which of them were lies. This is that vehicle-shaped
// bundle: what rolls (FChassis) and what the inspector calls it, nothing that flies.
//
// THE SIBLING OF FAirframe, NOT A BASE OF IT. Both hold an FChassis by composition, and
// FRoadAgent holds one of each and answers Chassis() from whichever it was started with - see
// FRoadAgent::Body. An articulated rig's trailer lands HERE, which is the point of the split:
// on FAirframe it would have handed every A380 a kingpin.
//
// ENFORCED BY: Check-Architecture rule 15 - this header may not name FAirframe.

#include "CoreMinimal.h"
#include "Model/Chassis.h"
#include "Vehicle.generated.h"

class UAnimInstance;
class USkeletalMesh;

/**
 * ONE LINK OF A TOW, when a vehicle pulls one (spec 2026-09-23 §6; a CHAIN since the revision
 * of 2026-09-24, §4). Measured from the model, uu.
 *
 * WAS FTrailer, ONE SEMI-TRAILER, until the drawbar fuel trailer arrived: a towbar pivoting on
 * a steered front axle, then a body on a fixed rear one - two pursuits, not one. A semi-trailer
 * is the one-link case, a drawbar trailer two links, a baggage train N, and one walk steps all
 * of them (VehicleSweep::StepChain), so the router and the driver cannot disagree about any.
 *
 * AN EMPTY CHAIN MEANS NO TRAILER - a rigid vehicle - which is the default, so every vehicle
 * assembled before trailers existed stays rigid without being told. (FTrailer said the same
 * with KingpinToAxle == 0.)
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FTowLink
{
	GENERATED_BODY()

	/**
	 * Where this link couples, along the PREVIOUS body from that body's fixed axle - the
	 * tractor's for link 0. Positive ahead, NEGATIVE BEHIND: a fifth wheel sits ahead of the
	 * drive axle (+57.3 on the rig), a drawbar eye behind the rear axle (about -91). Was
	 * FTrailer::KingpinX.
	 */
	UPROPERTY(EditAnywhere) double HitchX = 0.0;

	/** Hitch to this link's own axle (tandem centre). Was FTrailer::KingpinToAxle. */
	UPROPERTY(EditAnywhere) double Length = 0.0;

	/**
	 * How far the link's body reaches ahead of the HITCH, and behind its axle. Were
	 * FTrailer::FrontAheadOfKingpin and RearBehindAxle. Both zero is a BAR - a towbar, which
	 * sweeps its width between hitch and axle and nothing more.
	 */
	UPROPERTY(EditAnywhere) double BodyFront = 0.0;
	UPROPERTY(EditAnywhere) double BodyRear = 0.0;

	/** Body width, mirrors excluded. */
	UPROPERTY(EditAnywhere) double Width = 0.0;

	/**
	 * What this link LOOKS like - FVehicle::Mesh's sibling, PER LINK, since a tow's own body
	 * (or a future second rig's) is a different mesh from its cab and from any other link.
	 * Null for a bar (see IsBar): there is nothing of its own to draw, so nothing to name here.
	 *
	 * ADDED BY #308 to collapse UAirsideSettings::ResolveVehicleViewFor's TypeCode ladder: a
	 * link authored on a UVehicleType (or filled by ResolveRigVehicle/ResolveUtilityTowVehicle
	 * for now - see their own comments) names its own look here instead of a second function
	 * having to read a separate content field for it.
	 */
	UPROPERTY(EditAnywhere) TSoftObjectPtr<USkeletalMesh> Mesh;

	/** The anim Blueprint that drives Mesh. Null leaves the link in its reference pose. */
	UPROPERTY(EditAnywhere) TSoftClassPtr<UAnimInstance> AnimClass;

	/**
	 * A towbar: no body of its own, so nothing to draw for it. NAMED rather than tested inline
	 * at each site, because the view asks it twice - to skip a mesh, and to find the bar that
	 * swings a drawbar body's front axle (UAirsideTraffic::SpawnView).
	 */
	bool IsBar() const { return BodyFront == 0.0 && BodyRear == 0.0; }
};

/**
 * Every fact about one service vehicle that a dispatch needs, bundled - FAirframe's rule
 * ("one struct, not four parameters") applied to the other kind of thing on the apron.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FVehicle
{
	GENERATED_BODY()

	/** How it rolls. See FChassis. */
	UPROPERTY(EditAnywhere) FChassis Chassis;

	/**
	 * What the inspector SAYS this is - "FUEL". The vehicle counterpart of
	 * FAirframe::TypeCode, and here for the same reason: Model/ may not see Entities/, and the
	 * agent carries no pointer to a type. NAME_None for a vehicle assembled by hand.
	 */
	UPROPERTY(EditAnywhere) FName TypeCode;

	/**
	 * What this vehicle LOOKS like - FAirframe::Mesh's sibling for the other kind of thing on
	 * the apron. Null falls back to UAirsideSettings::ResolveVehicleView() - the game-wide
	 * default vehicle look - exactly as an aircraft with no Mesh of its own falls back to
	 * UAirsideContent::AgentMesh inside ResolveAgentView.
	 *
	 * ADDED BY #308, replacing a TypeCode STRING LADDER in
	 * UAirsideSettings::ResolveVehicleViewFor ("if TypeCode == RIG, return ResolveRigView()...")
	 * that grew by one branch, one content-field pair and one ENFORCED BY test per vehicle
	 * added to dispatch. UVehicleType::Vehicle() fills this from an authored asset; the rig and
	 * the utility tow (no asset yet, #287) fill it in code for now - see
	 * UAirsideSettings::ResolveRigVehicle's own comment.
	 */
	UPROPERTY(EditAnywhere) TSoftObjectPtr<USkeletalMesh> Mesh;

	/** The anim Blueprint that drives Mesh. Null leaves the vehicle in its reference pose. */
	UPROPERTY(EditAnywhere) TSoftClassPtr<UAnimInstance> AnimClass;

	/**
	 * The body's footprint, uu, measured from the model (spec 2026-09-23 §6): width over the
	 * body with MIRRORS EXCLUDED - they sit above a kerb and overhang it legally - and how far
	 * the body reaches ahead of and behind the FIXED axle (the chassis origin; RearX negative).
	 * What route search gates a vehicle on: its width in a lane, and through VehicleSweep its
	 * swept path round a corner. BodyWidth 0 means UNMEASURED and gates nothing.
	 */
	UPROPERTY(EditAnywhere) double BodyWidth = 0.0;
	UPROPERTY(EditAnywhere) double BodyFrontX = 0.0;
	UPROPERTY(EditAnywhere) double BodyRearX = 0.0;

	/**
	 * What it pulls, link by link from the tractor back, if anything. See FTowLink: empty, and
	 * so rigid, by default.
	 */
	UPROPERTY(EditAnywhere) TArray<FTowLink> Tow;

	bool HasTrailer() const { return Tow.Num() > 0; }

	/** The widest part of the vehicle - what a lane must take. */
	double WidestBody() const
	{
		double Widest = BodyWidth;
		for (const FTowLink& Link : Tow)
		{
			Widest = FMath::Max(Widest, Link.Width);
		}
		return Widest;
	}
};
