#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Tool/RoadBuildTool.h"
#include "RoadBuildHUD.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;

/**
 * Draws the road graph, and rasterises whatever the active tool says it intends.
 *
 * Two jobs, deliberately separated. The GRAPH is this class's own view of the model: a
 * ring per live node, coloured by degree, because a junction and a straight-through node
 * produce identical asphalt and a node with no segments draws nothing at all. The INTENT
 * belongs to the tool, which describes it in road-plane coordinates through
 * IToolPreviewSink and knows nothing about cameras, projection or colour.
 *
 * That split is what lets the tools live in the plugin. A tool that called into this class
 * would make the plugin depend on the game module, which it must never do.
 *
 * Screen space rather than world geometry because a marker's job is to stay readable at any
 * zoom. World-unit markers have to be sized against the road, so they swamp it zoomed in
 * and fall under a pixel zoomed out.
 *
 * Set this as HUD Class on the game mode.
 */
UCLASS()
class AIRPORTMGR_API ARoadBuildHUD : public AHUD, public IToolPreviewSink
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
	 * marker of its own it is invisible.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor StubColour = FLinearColor(1.0f, 0.55f, 0.1f);

	/** One or two incident segments: a dead end, or a straight-through node. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor EndColour = FLinearColor(0.85f, 0.85f, 0.85f);

	/** Three or more incident segments - a real junction, with a solved boundary. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Nodes")
	FLinearColor JunctionColour = FLinearColor(0.15f, 0.85f, 1.0f);

	/** Draw a marker at every placed stand's anchors, and the way it faces. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Stands")
	bool bDrawStands = true;

	/** The aircraft stop position - the thing a stand IS. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Stands")
	FLinearColor StandColour = FLinearColor(0.25f, 0.7f, 1.0f);

	/** Where the service vehicles park. Consequences of where the aircraft sits. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Stands")
	FLinearColor ServiceAnchorColour = FLinearColor(0.9f, 0.6f, 0.2f);

	/** Radius of a service anchor's ring, in pixels. Smaller than a road node's. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Stands", meta = (ClampMin = "1.0"))
	float ServiceAnchorRadius = 5.0f;

	/** Label each anchor with its id. Off by default; a stand carries eight of them. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Stands")
	bool bDrawAnchorIds = false;

	// --- Preview palette --------------------------------------------------------------
	//
	// One colour per EPreviewStyle. A tool names a MEANING and this maps it to a look, so
	// the plugin holds no colours, the palette can be retuned without touching a tool, and
	// every tool reads the same way for the same meaning.

	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	FLinearColor PendingColour = FLinearColor(0.2f, 1.0f, 0.3f);

	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	FLinearColor SnapColour = FLinearColor(1.0f, 0.9f, 0.15f);

	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	FLinearColor DoomedColour = FLinearColor(1.0f, 0.15f, 0.1f);

	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	FLinearColor HealColour = FLinearColor(0.3f, 1.0f, 0.5f);

	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	FLinearColor RefusedColour = FLinearColor(1.0f, 0.25f, 0.2f);

	/** Thickness of preview lines, in pixels. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview", meta = (ClampMin = "0.5"))
	float PreviewThickness = 3.0f;

	/** Half-length of a cross mark, in pixels. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview", meta = (ClampMin = "1.0"))
	float CrossMarkRadius = 9.0f;

	/**
	 * Name the active tool on screen.
	 *
	 * This is the first genuinely modal thing in the build tool, and a mode you cannot see
	 * is the classic modal trap: one click means two different things and nothing says which.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Preview")
	bool bDrawToolName = true;

	virtual void DrawHUD() override;

	// --- IToolPreviewSink, taking ROAD PLANE coordinates ------------------------------
	virtual void Marker(const FVector2D& At, EPreviewStyle Style) override;
	virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override;
	virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override;
	virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override;

private:
	/** The controller this HUD belongs to, if it is the road build controller. */
	ARoadBuildController* GetBuildController() const;

	/** Rings for every live node, coloured by degree. This class's own view of the model. */
	void DrawNodes(const ARoadNetworkActor& Target);

	/** Placed stands: the stop position, its anchors, and which way it faces. */
	void DrawStands(const ARoadNetworkActor& Target);

	FLinearColor StyleColour(EPreviewStyle Style) const;

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

	/**
	 * The road plane's height, cached at the top of DrawHUD.
	 *
	 * The sink methods take a plane position and nothing else - a tool has no business
	 * knowing what height the pavement sits at - so the one piece of world context they
	 * need is held here rather than threaded through the interface.
	 */
	double PlaneZ = 0.0;
};
