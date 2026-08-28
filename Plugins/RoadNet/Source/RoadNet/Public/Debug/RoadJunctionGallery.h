#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoadJunctionGallery.generated.h"

class URoadNetwork;
class URoadProfile;

/**
 * Slice 1 visual regression harness: builds every junction configuration the
 * solver must survive, side by side, and debug-draws the solved boundaries.
 */
UCLASS()
class ROADNET_API ARoadJunctionGallery : public AActor
{
	GENERATED_BODY()

public:
	ARoadJunctionGallery();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Spacing between gallery cells, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double CellSpacing = 50000.0;

	/**
	 * Arm length within each cell, in uu. Must exceed the largest cut distance the
	 * gallery produces or the arms render inside-out: the fillet reach is
	 * |R/tan(Theta/2)|, so the 15-degree cell alone cuts at ~12,533 uu with the
	 * default 1500 uu radius. The draw code clamps per arm as a backstop.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double ArmLength = 20000.0;

	UPROPERTY(EditAnywhere, Category = "RoadNet") double TaxiwayWidth = 2300.0;
	UPROPERTY(EditAnywhere, Category = "RoadNet") double FilletRadius = 1500.0;

private:
	void BuildGallery();

	UPROPERTY() TObjectPtr<URoadNetwork> Network;
	UPROPERTY() TObjectPtr<URoadProfile> Profile;

	/** Centre node of each gallery cell. */
	TArray<FVector2D> CellCentres;
	TArray<TArray<double>> CellBearings;
};
