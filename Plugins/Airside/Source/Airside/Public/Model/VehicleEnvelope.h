#pragma once

#include "CoreMinimal.h"
#include "Model/Chassis.h"
#include "Model/Vehicle.h"

enum class EIcaoCode : uint8;

/**
 * Every axis a stand's geometry is drawn against, each at its MAXIMUM over a set of vehicles -
 * what a stand that admits all of them must be laid for, and the ceiling a vehicle is admitted
 * under. User ruling 2026-10-03: "A stand should not refuse the lower vehicles; they are just less
 * efficient at doing their job" - a Code C stand is served by the utility tow as well as the truck
 * it was designed for, so its lanes are laid for BOTH.
 *
 * ONE STRUCT FOR THE LAYOUT AND THE ADMISSION, which is the point of it. Until 2026-10-03 the
 * layout read one design vehicle's figures and admission compared against that same vehicle with
 * VehicleFit::NoLargerThan, strict on chain length - so the tow (chain 575) was refused every C-F
 * stand laid for the truck (355), and the honest fix needed BOTH halves to move together: dropping
 * the chain axis alone would have sent a tow onto lanes with no straight for its trailer to settle
 * on. UEntityDefinition::BuildStandTemplate lays from FVehicleEnvelope::Of(the set) and
 * UJobBoard's bid admits through the same Of(the same set), so the two cannot disagree.
 * ENFORCED BY: Airside.Entities.EveryTemplateLegIsDrivableByEveryVehicle (drives every vehicle
 * the envelope admits on every letter's legs), Airside.Content.StandDesignVehicle.EveryLetterAdmitsEverySmallerLetter
 *
 * PER AXIS, NOT "NoLargerThan SOME MEMBER": the layout takes each figure from whichever vehicle
 * needs most of it - the truck's forward radius and the tow's settle straight on one Code C lane -
 * so a vehicle that is within every maximum is within what was laid, even when no single member is
 * larger than it on all four axes. That is the tow on a C stand: narrower and tighter-turning than
 * the truck, longer in the chain.
 */
struct AIRSIDE_API FVehicleEnvelope
{
	/** The widest body (FVehicle::WidestBody). Gates admission only - a stand's lane width is IcaoCode's. */
	double Width = 0.0;

	/** The widest forward turning circle (FChassis::TightestFollowableRadius) - every forward corner's radius. */
	double ForwardRadius = 0.0;

	/** The widest reverse turning circle (VehicleFit::TightestReverseRadius) - the reverse leg's corner. */
	double ReverseRadius = 0.0;

	/**
	 * The longest chain, steered axle to rearmost axle (VehicleFit::ChainLength), of a RIGID member,
	 * or zero when none is rigid. Its own axis for the reason TrailerChain is (review 2026-10-03): one
	 * Chain maxed over every member let a rigid kind as long as the tow's 575 onto C-F lanes laid for
	 * a 355 truck and a 575 TOW - no member there was a rigid that long.
	 */
	double RigidChain = 0.0;

	/**
	 * The longest chain of a member that TOWS, or zero when none does. A SEPARATE AXIS from Chain
	 * because only a trailer needs the settle straight (UEntityDefinition's TowSettleChains) - a
	 * rigid truck's 355 says nothing about how much straight a trailer needs, so a towing kind is
	 * admitted only up to the longest towing chain the lanes were settled for, never up to a rigid
	 * member's wheelbase.
	 */
	double TrailerChain = 0.0;

	/**
	 * The chassis whose forward radius IS ForwardRadius - for a consumer that sizes by a chassis
	 * rather than a figure (FAnchorLink::ServiceLaneRadius, a stand's road joins).
	 */
	FChassis WidestTurning;

	/** True for the envelope of nothing - admits nothing, lays nothing. */
	bool bEmpty = true;

	/** The per-axis maxima over Vehicles. Empty in, empty out. */
	static FVehicleEnvelope Of(TConstArrayView<FVehicle> Vehicles);

	/**
	 * Whether Kind is within every maximum - may serve a stand laid for this envelope. A towing
	 * Kind's chain is held to TrailerChain, a rigid one's to RigidChain - never to the other kind's.
	 * An empty envelope admits nothing.
	 */
	bool Admits(const FVehicle& Kind) const;
};

namespace VehicleEnvelope
{
	/**
	 * THE RULE, in one place: what a stand of Letter admits - its own letter's design vehicle
	 * FIRST, then every SMALLER letter's (user ruling 2026-10-03; "Vehicle <-> service is
	 * many-to-many", 2026-09-23), each TypeCode once. DesignOf answers one letter's design vehicle;
	 * the two callers hand in their own table (UAirsideSettings::ResolveStandDesignVehicle for the
	 * template and the content resolve, UJobBoard's letter table for a stand with no definition),
	 * so the rule is not typed twice. Letters are walked by declaration order, A..Letter.
	 * ENFORCED BY: Airside.Content.StandDesignVehicle.EveryLetterAdmitsEverySmallerLetter
	 */
	AIRSIDE_API TArray<FVehicle> AdmittedUpTo(EIcaoCode Letter, TFunctionRef<FVehicle(EIcaoCode)> DesignOf);

	/**
	 * First followed by Rest, First at index 0 and each TypeCode once - how a definition's authored design
	 * vehicle RAISES its letter's set and never lowers it (a vehicle smaller letters admit stays
	 * admitted, whatever the definition names).
	 */
	AIRSIDE_API TArray<FVehicle> WithDesignFirst(const FVehicle& First, TConstArrayView<FVehicle> Rest);
}
