#pragma once

#include "CoreMinimal.h"
#include "BuildCameraRig.h"
#include "GameFramework/PlayerController.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "RoadBuildController.generated.h"

class ARoadNetworkActor;

/**
 * Lets the player build the road graph while the game runs: click to drop a node,
 * click again to run a segment to it, chaining as you go.
 *
 * This is the minimum needed to exercise the model -> solver -> mesh pipeline live. It
 * is NOT the build tool of design spec section 7 - there is no state machine, no
 * IRoadCommand, no undo and no validation. It DOES drive the section 7.4 snap chain,
 * which is what lets a click reuse a node or split a segment. Slice 3 replaces this
 * class outright; it survives only because the facade it calls on ARoadNetworkActor is
 * the same one commands will drive.
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
	 *
	 * This is the snap chain's rule 1 radius; it keeps its old name because it is the same
	 * number it always was.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0"))
	double PickRadius = 150.0;

	/** How close a click must land to split an existing segment, in uu. Snap rule 2. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Snap", meta = (ClampMin = "0.0"))
	double SegmentSnapRadius = 150.0;

	/**
	 * Let a click land on a segment and split it.
	 *
	 * Off, a junction can only ever form where a node was already placed, so a road run
	 * into one already drawn just crosses over it.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Snap")
	bool bSnapToSegments = true;

	/** Nearest a split may happen to the ends of the segment being split, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Snap", meta = (ClampMin = "0.0"))
	double MinSplitFromEndpoint = 50.0;

	/**
	 * Furthest a click may place a node, as a MULTIPLE of the current view distance.
	 *
	 * The ray/plane distance is (SurfaceZ - Origin.Z) / Direction.Z, which runs away
	 * towards infinity as a click approaches the horizon - and the horizon is on screen
	 * now that the view is angled. A click a few pixels too high lands kilometres out.
	 *
	 * Relative rather than absolute because the view spans a hundredfold range of
	 * distances: a fixed cap that allows a legitimate click when zoomed out would let a
	 * horizon click through when zoomed in, and one tight enough for the close view would
	 * reject half the screen when zoomed out.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0"))
	double MaxPlaceDistanceFactor = 6.0;

	/**
	 * Use the orbiting build camera instead of the pawn's own view.
	 *
	 * Viewing through a camera actor also takes the view away from the pawn, so the pawn's
	 * mouse-look stops fighting the cursor for the same input.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View")
	bool bStartAbovePlane = true;

	/** Camera-to-focus distance the session opens at, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0"))
	double StartViewDistance = 8000.0;

	/** Closest the camera may come, in uu. Sized to sit beside a vehicle. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0"))
	double MinViewDistance = 600.0;

	/** Furthest the camera may pull back, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0"))
	double MaxViewDistance = 60000.0;

	/** Pitch at MinViewDistance, in degrees below horizontal. Near eye level. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MinPitchDegrees = 30.0;

	/**
	 * Pitch at MaxViewDistance. 90 would be straight down, and is deliberately not
	 * offered: control rotation renormalises unpredictably at the poles, and a view that
	 * flat loses every cue about relief that the angle exists to provide.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MaxPitchDegrees = 70.0;

	/** Fraction the view distance changes per mouse-wheel notch. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "0.01", ClampMax = "0.9"))
	double ZoomStep = 0.15;

	/**
	 * Pan speed, in view distances per second.
	 *
	 * Not uu per second: the view spans a hundredfold range, and a fixed speed crawls when
	 * zoomed out and overshoots when zoomed in. As a fraction of the view, a pan crosses
	 * the same amount of screen at every zoom.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "0.0"))
	double PanRate = 0.9;

	/** Rotation speed on Q and E, in degrees per second. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "0.0"))
	double RotateRate = 90.0;

	/** Seconds the view takes to settle after an input. Zero snaps. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "0.0"))
	double CameraLag = 0.12;

	/** Horizontal field of view, in degrees. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|View", meta = (ClampMin = "20.0", ClampMax = "150.0"))
	double FieldOfView = 75.0;

	/**
	 * Show the ghost of the segment the next click would build.
	 *
	 * Real solved pavement on a duplicate of the graph, not a rubber band: it carries the
	 * road's actual width and the shape the junction at either end will take.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	bool bDrawBuildPreview = true;

	/** Shortest segment a click may build, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Placement", meta = (ClampMin = "0.0"))
	double MinSegmentLength = 250.0;

	/** Tightest corner a click may make against a road already leaving the start node. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Placement", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	double MinTurnDegrees = 25.0;

	// --- Read side, for ARoadBuildHUD ------------------------------------------------
	//
	// The overlay draws what this controller has decided; it must not decide anything
	// itself. Exposing the decisions rather than the state is what keeps the two from
	// drifting into disagreeing about which node the next click will use.

	/** The road actor being built into, or null when the level has none. */
	ARoadNetworkActor* GetTarget() const { return Target; }

	/** Node the next click runs a segment from, or INDEX_NONE when not chaining. */
	int32 GetPendingNode() const { return PendingNode; }

	/**
	 * What the next click would do, run through the snap chain. False only when the
	 * cursor is not over the road plane at all.
	 *
	 * The single source of truth for the decision: the overlay draws this and OnBuildClick
	 * acts on it, so what is highlighted and what happens cannot disagree.
	 */
	bool ResolveSnap(FRoadSnapResult& Out, bool bLogRefusals = false) const;

	/**
	 * Where the cursor meets the road plane.
	 *
	 * An exact ray/plane intersection, not a line trace: design spec section 6.2 states
	 * the roads carry no collision, on the grounds that the world is flat so the maths
	 * is exact and generating collision purely to support mouse picking would be waste.
	 */
	bool CursorOnRoadPlane(FVector2D& OutPosition, bool bLogRefusals = false) const;

	/**
	 * Whether the click being previewed may be built, and why not if it may not.
	 *
	 * False when nothing is being previewed at all - no chain in progress, or the cursor
	 * off the road plane. Computed once in PlayerTick and read back here, so the ghost's
	 * colour, the overlay's reason text and the click's own decision are one value rather
	 * than three that agree by coincidence.
	 */
	bool GetPendingPlacement(ERoadPlacement& Out) const;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

private:
	void OnBuildClick();
	void OnCancelChain();
	void OnClearNetwork();

	/** World-space position of a node, at the road plane's height. */
	bool NodeWorldLocation(int32 NodeIndex, FVector& OutLocation) const;

	/** Radii and toggles above, gathered into the form the chain takes. */
	FRoadSnapSettings MakeSnapSettings() const;

	FRoadPlacementLimits MakePlacementLimits() const;

	/** Judge the snap against PendingNode. Valid when no chain is in progress. */
	ERoadPlacement JudgePlacement(const FRoadSnapResult& Snap) const;

	void CreateBuildCamera();

	/** Read WASD/QE into the target rig, ease the view towards it, and apply it. */
	void UpdateView(float DeltaTime);

	/** Mouse wheel. Moves the camera in or out; the pitch follows from the distance. */
	void ZoomIn();
	void ZoomOut();
	void ZoomBy(double Notches);

	/** Copy the tunables above onto a rig, so details-panel edits take effect live. */
	void ApplyViewLimits(FBuildCameraRig& Rig) const;

	/** Orbiting camera spawned on possession; the view target while building. */
	UPROPERTY(Transient) TObjectPtr<class ACameraActor> BuildCamera;

	/** Where the input says the view should be, and where it actually is. Separate so a
	 *  wheel notch eases in rather than cutting - see FBuildCameraRig::EaseToward. */
	FBuildCameraRig TargetView;
	FBuildCameraRig CurrentView;

	/** Resolved once on BeginPlay; the first ARoadNetworkActor in the level. */
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/** Node the next click will run a segment from, or INDEX_NONE when not chaining. */
	int32 PendingNode = INDEX_NONE;

	/**
	 * Rule 1 then rule 2, in that order. Not a UPROPERTY: it owns its rules through
	 * TUniquePtr and holds no state worth saving, only the ordering.
	 */
	FRoadSnapChain SnapChain;

	/** Last judgement made in PlayerTick, and whether it referred to anything. */
	ERoadPlacement LastPlacement = ERoadPlacement::Valid;
	bool bLastPlacementRelevant = false;
};
