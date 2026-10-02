#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AirsideOwnedLandActor.generated.h"

class UStaticMeshComponent;
class UWorld;

/**
 * The land the player owns, and the diorama edge that shows where it ends (2026-10-02): the
 * landscape is clipped to it, the plinth's strata walls stand under its sides, the build camera's
 * focus stays inside it and the grass grows only on it. ONE RECTANGLE, ONE SOURCE OF TRUTH - each
 * of those four reads it from here, so they cannot drift apart. The look was chosen over a sea or
 * mountain backdrop and over a farmland surround (env spec section 10, Slice D).
 *
 * PLACED IN THE LEVEL, not spawned: the rectangle is a fact about the map, like the landscape.
 * A level with none owns everything - nothing is clipped, bounded or walled - which is every map
 * made before this existed.
 *
 * WHY THE LANDSCAPE IS CLIPPED RATHER THAN SIZED: a Landscape cannot be created or resized from
 * script or at runtime (env spec 7.2), and land purchase will grow this rectangle in play. A
 * material clip on a landscape that already covers the whole map grows with one parameter write.
 *
 * Land purchase is not built yet. When it is, it calls SetOwnedLand, which rewrites the collection
 * and the walls here; the camera and grass will need telling too (they read this once - see
 * UBuildCameraComponent::CreateBuildCamera and AAirsideGroundCoverActor::RebuildMask).
 */
UCLASS()
class AIRSIDE_API AAirsideOwnedLandActor : public AActor
{
	GENERATED_BODY()

public:
	AAirsideOwnedLandActor();

	/** The owned land's corners on the road plane, uu. Min must be below Max on both axes. */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand")
	FVector2D OwnedMin = FVector2D(-30000.0, -30000.0);

	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand")
	FVector2D OwnedMax = FVector2D(30000.0, 30000.0);

	/**
	 * How far the plinth's walls reach below the ground, uu. 40 m is exaggerated on purpose - a
	 * diorama base, not geology - and was judged on 20/150/600 m shots (2026-10-02). From 600 m it
	 * reads thin; deepen it here if the edge must read from the top zoom.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "100.0"))
	double PlinthDepth = 4000.0;

	/** Wall thickness, uu. Only its outer face shows; thick enough not to z-fight its own back. */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "1.0"))
	double WallThickness = 100.0;

	/** The rectangle, valid only when Min < Max on both axes. */
	FBox2D GetOwnedLand() const;

	/** Move the edge: walls and the ground clip follow at once. A UFUNCTION so the authoring script
	 *  can set it headlessly, where a property write alone reruns no construction. */
	UFUNCTION(BlueprintCallable, Category = "Airside|OwnedLand")
	void SetOwnedLand(const FVector2D& InMin, const FVector2D& InMax);

	/** The level's owned land, or null when it has none - which means "owns everything". */
	static AAirsideOwnedLandActor* Find(const UWorld* World);

	/** The four walls, south/north/west/east - exposed for the test that measures them. */
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetWalls() const { return Walls; }

	/** Names of the four MPC_OwnedLand scalars, in the order Min.X, Min.Y, Max.X, Max.Y. */
	static const FName CollectionParams[4];

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

private:
	/** Lay the walls under the rectangle's edges and write the clip. Editor and game alike. */
	void Apply();

	/** Write Bounds to MPC_OwnedLand in this world - or "everything" when Bounds is invalid. */
	void WriteCollection(const FBox2D& Bounds) const;

	UPROPERTY(VisibleAnywhere, Category = "Airside|OwnedLand")
	TArray<TObjectPtr<UStaticMeshComponent>> Walls;
};
