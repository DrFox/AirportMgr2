#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "RoadBuildHUD.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;

/**
 * Draws the road graph's nodes as a screen-space overlay.
 *
 * The graph's joints are the one thing the pavement mesh cannot show you. A junction and
 * a straight-through node produce the same continuous asphalt, and a node with no
 * segments yet renders nothing at all - SolveAll skips it - so before this the only
 * evidence a click had landed was a log line. Segments are deliberately NOT drawn: the
 * mesh and its centreline already are the segment.
 *
 * Screen space rather than world geometry because a marker's job is to stay readable at
 * any zoom. World-unit markers have to be sized against the road, so they swamp it zoomed
 * in and fall below a pixel zoomed out - the same sub-pixel trap that made earlier debug
 * lines indistinguishable from nothing being drawn.
 *
 * Set this as HUD Class on the game mode. It reads ARoadBuildController for the target
 * actor and the pending node, and draws nothing when there is no road actor in the level.
 */
UCLASS()
class AIRPORTMGR_API ARoadBuildHUD : public AHUD
{
	GENERATED_BODY()

public:
	/** Draw a ring at every live node. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	bool bDrawNodes = true;

	/** Ring radius in PIXELS, so it is the same size however far the view is zoomed out. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes", meta = (ClampMin = "1.0"))
	float NodeRingRadius = 9.0f;

	/** Line thickness of a ring, in pixels. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes", meta = (ClampMin = "0.5"))
	float NodeRingThickness = 2.0f;

	/** Sides of the polygon a ring is drawn as. Below about 10 it reads as a polygon. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes", meta = (ClampMin = "3", ClampMax = "64"))
	int32 NodeRingSides = 16;

	/** Label each ring with its node index. Off by default; it clutters a dense graph. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	bool bDrawNodeIndices = false;

	/**
	 * A node with no incident segments. It draws no pavement whatsoever, so without a
	 * marker of its own it is invisible - which is what makes the first click of a chain
	 * look like a no-op.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor StubColour = FLinearColor(1.0f, 0.55f, 0.1f);

	/** One or two incident segments: a dead end, or a straight-through node. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor EndColour = FLinearColor(0.85f, 0.85f, 0.85f);

	/** Three or more incident segments - a real junction, with a solved boundary. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor JunctionColour = FLinearColor(0.15f, 0.85f, 1.0f);

	/** The node the next click runs a segment from. Drawn as a double ring. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor PendingColour = FLinearColor(0.2f, 1.0f, 0.3f);

	/** A node the next click would reuse rather than add to. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor SnapColour = FLinearColor(1.0f, 0.9f, 0.15f);

	/** A segment the next click would cut in two. Distinct from SnapColour on purpose:
	 *  reusing a node and cutting a new one into a road are different edits. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor SplitColour = FLinearColor(1.0f, 0.4f, 0.8f);

	/**
	 * What a Ctrl+click would remove: the node ring, and a line along every segment that
	 * would cascade with it.
	 *
	 * This is the ONLY place the overlay draws segments. Slice A left them out because the
	 * pavement already shows where the roads are - but it cannot show WHICH ones are about
	 * to go, and a delete that takes more than the thing under the cursor has to say so
	 * before the click, not after.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Delete")
	FLinearColor DoomedColour = FLinearColor(1.0f, 0.15f, 0.1f);

	/** The roads a deletion would create to rejoin what it strands. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Delete")
	FLinearColor HealColour = FLinearColor(0.3f, 1.0f, 0.5f);

	/** Thickness of the doomed-segment lines, in pixels. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Delete", meta = (ClampMin = "0.5"))
	float DoomedThickness = 3.0f;

	/** Text colour for the reason a click will be refused. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor RefusedColour = FLinearColor(1.0f, 0.25f, 0.2f);

	/** Draw a crosshair where the cursor meets the road plane. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Cursor")
	bool bDrawCursor = true;

	/** Half-length of the cursor crosshair's arms, in pixels. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Cursor", meta = (ClampMin = "1.0"))
	float CursorSize = 7.0f;

	virtual void DrawHUD() override;

private:
	/** The controller this HUD belongs to, if it is the road build controller. */
	ARoadBuildController* GetBuildController() const;

	/** Snap is what the next click would do; a node it names is drawn highlighted. */
	void DrawNodes(const ARoadNetworkActor& Target, int32 PendingNode, const FRoadSnapResult& Snap);

	/**
	 * Where the click will actually land - the SNAPPED position, not the raw cursor, so
	 * the marker separates from the mouse pointer exactly when a snap has moved it. For a
	 * split it also draws the cut across the segment.
	 */
	void DrawSnapMarker(const ARoadNetworkActor& Target, const FRoadSnapResult& Snap);

	/** Redden what a Ctrl+click would remove, cascade included. */
	void DrawDoomed(const ARoadNetworkActor& Target, const FRoadSnapResult& Snap);

	/** A ring on a node by slot index, in a colour of its own. */
	void DrawNodeRing(const ARoadNetworkActor& Target, int32 NodeIndex,
		const FLinearColor& Colour, float Thickness);

	/** A straight line between two nodes - a road that does not exist yet. */
	void DrawPlaneLine(const ARoadNetworkActor& Target, FRoadNodeId From, FRoadNodeId To,
		const FLinearColor& Colour, float Thickness);

	/** A line along a segment, in screen space. Skipped if either end cannot be drawn. */
	void DrawSegmentLine(const ARoadNetworkActor& Target, int32 SegmentIndex,
		const FLinearColor& Colour, float Thickness);

	/** Ring of NodeRingSides segments, centred on a screen position. */
	void DrawRing(const FVector2D& Centre, float Radius, const FLinearColor& Colour, float Thickness);

	/**
	 * Screen position of a point on the road plane. False when it cannot be drawn.
	 *
	 * Rejects Project()'s behind-the-camera sentinel - an exact zero, written when the clip
	 * W goes non-positive - and culls everything else on screen bounds. It does NOT test
	 * `Z <= 0`, and must not be "simplified" into doing so: Z is a depth that is
	 * legitimately near zero far from the camera, and an orthographic projection leaves W
	 * at 1 for every point so the sentinel never fires there at all. The build view is
	 * perspective today, which makes the sentinel live and this correct; written this way
	 * it stays correct if an orthographic mode returns, where `Z <= 0` would silently drop
	 * every node on screen.
	 */
	bool ProjectPlanePoint(const FVector2D& Where, double SurfaceZ, FVector2D& OutScreen) const;
};
