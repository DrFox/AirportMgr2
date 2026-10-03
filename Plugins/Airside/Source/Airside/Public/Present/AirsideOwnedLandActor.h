#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AirsideOwnedLandActor.generated.h"

class UStaticMeshComponent;
class URoadEditFacade;
class UWorld;
struct FLandGrid;

/**
 * The diorama edge (2026-10-02): DRAWS the land the player owns - the landscape clipped to it, and the plinth's
 * strata walls standing under its boundary. The look was chosen over a sea or mountain backdrop and over a farmland
 * surround (env spec section 10, Slice D).
 *
 * IT DRAWS THE LAND; IT DOES NOT HOLD IT. The owned tiles are FLandGrid on the airport's URoadNetwork (land purchase
 * spec 2026-10-02 section 2), written only by URoadEditFacade, saved with the level and every save. #529 kept a
 * rectangle here; that was replaced rather than kept beside the tiles, because two descriptions of owned land is the
 * drift the one rectangle existed to prevent. The camera, the grass and every build refusal read the same grid.
 *
 * PLACED IN THE LEVEL. A level with none still has its land (refusals and the camera bound hold); it just has no
 * edge drawn. A level whose airport owns no grid owns everything, and this draws nothing.
 *
 * WHY THE LANDSCAPE IS CLIPPED RATHER THAN SIZED: a Landscape cannot be created or resized from script or at runtime
 * (env spec 7.2), and land purchase grows the land in play. A material clip on a landscape that already covers the
 * whole map grows with one parameter write.
 */
UCLASS()
class AIRSIDE_API AAirsideOwnedLandActor : public AActor
{
	GENERATED_BODY()

public:
	AAirsideOwnedLandActor();

	/**
	 * How far the plinth's walls reach below the ground, uu. 40 m is exaggerated on purpose - a diorama base, not
	 * geology - and was judged on 20/150/600 m shots (2026-10-02). From 600 m it reads thin; deepen it here if the
	 * edge must read from the top zoom.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "100.0"))
	double PlinthDepth = 4000.0;

	/** Wall thickness, uu. Only its outer face shows; thick enough not to z-fight its own back. */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "1.0"))
	double WallThickness = 100.0;

	/**
	 * Author this level's starting land on its airport - the level script's door (build_diorama_prototype.py). The
	 * land then lives on the network; this actor only draws it. A UFUNCTION so the script can call it headlessly,
	 * where a property write reruns no construction.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside|OwnedLand")
	void AuthorStartingLand(FVector2D Origin, double TileSize, int32 Columns, int32 Rows, const TArray<FIntPoint>& StartTiles);

	/** The level's edge actor, or null when it has none. */
	static AAirsideOwnedLandActor* Find(const UWorld* World);

	/** The pooled walls; only the first NumWalls() are live - exposed for the test that measures them. */
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetWalls() const { return Walls; }
	int32 NumWalls() const { return LiveWalls; }

	/**
	 * Names of the MPC_OwnedLand scalars: LandValid, the grid's origin, tile size and size, and the 64-bit mask as four
	 * 16-bit words (a float holds integers exactly to 2^24). LandValid 0 clips nothing.
	 */
	static const FName CollectionParams[10];

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

private:
	/** Draw Land: walls under every boundary run, and the clip written. Invalid land hides the walls and clips nothing. */
	void Apply(const FLandGrid& Land);

	/** Write Land to MPC_OwnedLand in this world. */
	void WriteCollection(const FLandGrid& Land) const;

	/** Listen to the airport's owned land, and draw it once to catch up. */
	void Bind();
	void Unbind();

	UStaticMeshComponent* WallAt(int32 Index);

	/** Pooled: an L-shape adds walls, a purchase that straightens an edge frees one. At most 4 x 32 runs on 8x8. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Airside|OwnedLand")
	TArray<TObjectPtr<UStaticMeshComponent>> Walls;

	int32 LiveWalls = 0;
	TWeakObjectPtr<URoadEditFacade> BoundFacade;
	FDelegateHandle BoundHandle;
};
