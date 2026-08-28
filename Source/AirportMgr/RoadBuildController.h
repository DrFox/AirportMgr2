#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "RoadBuildController.generated.h"

class ARoadNetworkActor;

/**
 * Lets the player build the road graph while the game runs: click to drop a node,
 * click again to run a segment to it, chaining as you go.
 *
 * This is the minimum needed to exercise the model -> solver -> mesh pipeline live. It
 * is NOT the build tool of design spec section 7 - there is no state machine, no
 * IRoadCommand, no undo, no validation, no ghost preview, and picking an existing node
 * is a plain radius search rather than the snap chain of section 7.4. Slice 3 replaces
 * this class outright; it survives only because the facade it calls on
 * ARoadNetworkActor is the same one commands will drive.
 *
 * It lives in the game module rather than the RoadNet plugin because a PlayerController
 * is game-framework glue. The plugin must not depend on the game.
 */
UCLASS()
class AIRPORTMGR_API ARoadBuildController : public APlayerController
{
	GENERATED_BODY()

public:
	ARoadBuildController();

	/**
	 * How close a click must land to reuse an existing node instead of adding one, in uu.
	 * Roughly the road's own width: much wider and the cursor snaps to junctions you were
	 * trying to draw past.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0"))
	double PickRadius = 600.0;

	/**
	 * Furthest a click may place a node from the camera, in uu.
	 *
	 * The ray/plane distance is (SurfaceZ - Origin.Z) / Direction.Z, which runs away
	 * towards infinity as a click approaches the horizon: from near ground level almost
	 * every ray is shallow, and a click a few pixels above the skyline lands kilometres
	 * out. Without this the first stray click builds a road the size of a county, and
	 * at that range the depth buffer cannot separate the surface from the ground either.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1.0"))
	double MaxPlaceDistance = 40000.0;

	/** Lift the pawn above the road plane on possession, looking down at it. */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	bool bStartAbovePlane = true;

	/**
	 * Height above the road plane to start at, in uu. Ignored unless bStartAbovePlane.
	 *
	 * Sets the drawing scale, because a click's world position is where its ray meets
	 * the plane: doubling this doubles how far apart two clicks land. Around 8000 uu
	 * puts a screen's width at roughly 17,000 uu, so an ordinary click-to-click distance
	 * comfortably clears the fillet reach at the default road width.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0"))
	double StartHeight = 8000.0;

	/** Draw the pending node and a rubber band to the cursor. */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	bool bDrawBuildPreview = true;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

private:
	void OnBuildClick();
	void OnCancelChain();
	void OnClearNetwork();

	/**
	 * Where the cursor meets the road plane.
	 *
	 * An exact ray/plane intersection, not a line trace: design spec section 6.2 states
	 * the roads carry no collision, on the grounds that the world is flat so the maths
	 * is exact and generating collision purely to support mouse picking would be waste.
	 */
	bool CursorOnRoadPlane(FVector2D& OutPosition) const;

	/** World-space position of a node, at the road plane's height. */
	bool NodeWorldLocation(int32 NodeIndex, FVector& OutLocation) const;

	void MoveViewAbovePlane();

	/** Resolved once on BeginPlay; the first ARoadNetworkActor in the level. */
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/** Node the next click will run a segment from, or INDEX_NONE when not chaining. */
	int32 PendingNode = INDEX_NONE;
};
