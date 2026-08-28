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

	/**
	 * Tick in the editor viewport as well as in play.
	 *
	 * This is a visual regression harness: needing to press Play to see it would
	 * make it useless for the one job it has. Ticking in the editor also means the
	 * gallery can be inspected from the editor camera, which is controllable, rather
	 * than from wherever PIE happens to spawn a pawn.
	 */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

#if WITH_EDITOR
	/** Discard the built gallery so an edited property takes effect on the next tick. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

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

	/**
	 * Debug line thickness in WORLD units. The whole gallery spans ~150,000 uu, so a
	 * few units is sub-pixel when it is all in frame. Raise this to see the gallery
	 * from above; drop it to ~6 to inspect a single junction up close.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double DebugLineThickness = 120.0;

private:
	void BuildGallery();

	UPROPERTY() TObjectPtr<URoadNetwork> Network;
	UPROPERTY() TObjectPtr<URoadProfile> Profile;

	/** Centre node of each gallery cell. */
	TArray<FVector2D> CellCentres;
	TArray<TArray<double>> CellBearings;
};
