#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/Vehicle.h"
#include "VehicleType.generated.h"

class UAnimInstance;
class USkeletalMesh;

/**
 * One ground vehicle type: what it looks like, what drives its bones, and the measured
 * geometry a dispatch gates it on. UAircraftType's counterpart for the other kind of thing
 * on the apron - the "M3 adds the type with the fleet" that
 * UAirsideSettings::ResolveDefaultVehicle has been promising.
 *
 * WHY AN ASSET PER TYPE AND NOT ANOTHER PROPERTY PAIR ON UAirsideContent. Until 2026-09-25 a
 * vehicle was two properties on the content set (VehicleSkeletalMesh and VehicleAnimClass),
 * which was right for exactly one vehicle and is a list that grows by editing a C++ header
 * for every one after it. The catering truck, the baggage cart and the curtain trailer made
 * three more in one go. An asset per type is what the aircraft already do, and it gives the
 * model yard and every future consumer ONE thing to enumerate - see AnimYardCatalogue.
 *
 * LOCAL SPACE is the vehicle's own: origin on the FIXED axle at the ground, +X forward - the
 * ground-vehicle convention every model in AirportMgr2Models shares (rigidCab1/README.md,
 * baggageCart1/README.md). A towed vehicle's fixed axle is its rear axle; a semi-trailer's is
 * its axle group's centre.
 *
 * NOT YET WHAT DISPATCH READS, and that is a named gap rather than an oversight. The fuel
 * truck's figures are still decided in UAirsideSettings::ResolveDefaultVehicle, next to the
 * paragraphs of reasons each one carries; moving them onto DA_Vehicle_FuelTruck1 is a
 * refactor of the dispatch path that also collides with feature/articulated-rig's
 * ResolveRigVehicle, which is built on it. Until then the two are held together by a test
 * rather than by a comment.
 * ENFORCED BY: AirportMgr.Content.VehicleTypes.FuelTruckAgreesWithDispatch
 */
UCLASS(BlueprintType)
class AIRSIDE_API UVehicleType : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * What the inspector SAYS this is - "FUEL", "CATR". Copied into FVehicle::TypeCode by
	 * Vehicle(), so it is stated here once rather than on both.
	 */
	UPROPERTY(EditAnywhere) FName Code;

	/** What a player reads - "Catering high-loader". */
	UPROPERTY(EditAnywhere) FText DisplayName;

	/** The rigged model. */
	UPROPERTY(EditAnywhere) TSoftObjectPtr<USkeletalMesh> Mesh;

	/**
	 * What drives Mesh's bones. Built on UAirsideAgentAnim, like every Animation Blueprint
	 * here. Null leaves the vehicle in its reference pose - it still draws, it just slides.
	 */
	UPROPERTY(EditAnywhere) TSoftClassPtr<UAnimInstance> AnimClass;

	/**
	 * How it rolls and how much room it takes: chassis, body footprint, trailer. MEASURED off
	 * the mesh's own bones - AirportMgr.Content.VehicleTypes.AxlesMatchTheMesh holds each
	 * type's axles to its skeleton, so a re-export that moves a wheel fails a test rather than
	 * leaving a vehicle whose wheels and route disagree.
	 *
	 * TypeCode is NOT read from here; Vehicle() writes Code over it.
	 */
	UPROPERTY(EditAnywhere) FVehicle Geometry;

	/**
	 * Towed, not driven: a baggage cart or a curtain trailer has no engine and goes nowhere on
	 * its own. The yard shows it rolling on the bench's shared motion like everything else,
	 * which is how its wheels are checked; the airport must not dispatch it alone, and a
	 * consumer that enumerates types to dispatch filters on this.
	 */
	UPROPERTY(EditAnywhere) bool bTowed = false;

	/**
	 * The dispatchable figures, with the type's Code stamped on.
	 *
	 * ASSEMBLED ON DEMAND, as UAircraftType::Airframe() is, because Geometry is the authored
	 * source of truth and a cached copy is a second place to drift.
	 */
	FVehicle Vehicle() const
	{
		FVehicle Out = Geometry;
		Out.TypeCode = Code;
		return Out;
	}
};
