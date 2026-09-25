#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Present/PreviewPalette.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/ToolReadout.h"
#include "RoadBuildHUD.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UUIStyle;

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
	UPROPERTY(EditAnywhere, Category = "Airside|Nodes")
	bool bDrawNodes = true;

	/** Ring radius in PIXELS, so it is the same size however far the view is zoomed out. */
	UPROPERTY(EditAnywhere, Category = "Airside|Nodes", meta = (ClampMin = "1.0"))
	float NodeRingRadius = 9.0f;

	/** Sides of the polygon a ring is drawn as. Below about 10 it reads as a polygon. */
	UPROPERTY(EditAnywhere, Category = "Airside|Nodes", meta = (ClampMin = "3", ClampMax = "64"))
	int32 NodeRingSides = 16;

	/** Label each ring with its node index. Off by default; it clutters a dense graph. */
	UPROPERTY(EditAnywhere, Category = "Airside|Nodes")
	bool bDrawNodeIndices = false;

	/** Draw a marker at every placed stand's anchors, and the way it faces. */
	UPROPERTY(EditAnywhere, Category = "Airside|Stands")
	bool bDrawStands = true;

	/** Label each anchor with its id. Off by default; a stand carries eight of them. */
	UPROPERTY(EditAnywhere, Category = "Airside|Stands")
	bool bDrawAnchorIds = false;

	// --- Preview palette --------------------------------------------------------------
	//
	// One FPreviewLook per EPreviewStyle: a tool names a MEANING and this maps it to a look
	// (colour, ring radius/thickness scale, whether it draws a second emphasis ring), so the
	// plugin holds no colours, the palette can be retuned without touching a tool or this
	// class's drawing code, and every tool reads the same way for the same meaning.
	//
	// Used to be 15 separate FLinearColor UPROPERTYs (one of them, IntermediateHolding-
	// PositionColour, even drifted into the wrong Category by hand) plus a StyleColour
	// switch plus a chain of if/else in Marker/Line picking ring radius, ring thickness and
	// line weight per style - three places a reader had to check against each other for
	// every style (#104). Seeded in the constructor from PreviewPalette::DefaultLook, the
	// same source the editor viewport's colours already came from - this UPROPERTY still
	// lets a designer override any one style's whole look per level afterwards.
	UPROPERTY(EditAnywhere, Category = "Airside|Preview")
	TMap<EPreviewStyle, FPreviewLook> Looks;

	/** Thickness of preview lines, in pixels. */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview", meta = (ClampMin = "0.5"))
	float PreviewThickness = 3.0f;

	/**
	 * Dash plus gap for a Provisional line, in PIXELS. Half is drawn, half is skipped.
	 *
	 * Screen space, not world space: a world-space dash shortens with distance until a far
	 * edge reads as a solid line, which is the one thing the dash exists to deny. Same reason
	 * CrossMark takes its length from this class rather than from the tool.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview", meta = (ClampMin = "4.0"))
	float DashPitch = 18.0f;

	/** Half-length of a cross mark, in pixels. */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview", meta = (ClampMin = "1.0"))
	float CrossMarkRadius = 9.0f;

	// The active tool's name used to be drawn here (a mode you cannot see is the classic modal
	// trap). It is the lit button on UBuildBarWidget now, so the flag that switched it is gone.

	ARoadBuildHUD();

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

	// --- IToolPreviewSink, taking ROAD PLANE coordinates ------------------------------
	virtual void Marker(const FVector2D& At, EPreviewStyle Style) override;
	virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override;
	virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override;
	virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override;

	/**
	 * Whether this style draws as a dashed line.
	 *
	 * STATIC AND PUBLIC so the one rule is testable with no Canvas. The PLUGIN never names a
	 * dash length - it names a MEANING, and this is where meaning becomes look, in the same
	 * class that turns a style into a colour.
	 */
	// TWO STYLES DASH, for two different reasons: Provisional because the edge has not
	// stopped moving, Guide because it is not an edge at all. They are told apart by COLOUR
	// - see PreviewPalette::Default - not by the dash they share.
	static bool IsDashed(EPreviewStyle Style)
	{
		return Style == EPreviewStyle::Provisional || Style == EPreviewStyle::Guide;
	}

	/**
	 * What to offer the player when a gesture is ready to commit - "Build  [Enter]" - or an
	 * empty string when nothing is.
	 *
	 * STATIC AND TAKING THE READOUT, so the one decision that matters is testable with no
	 * Canvas, no world and no PIE: that the prompt appears exactly when bCommittable is set,
	 * and that its key comes from BuildActions() rather than a literal typed here. A literal
	 * would be a second place naming the Build key, and CLAUDE.md records three separate
	 * occasions this project shipped a key that went nowhere.
	 */
	static FString CommitPromptText(const FToolReadout& Readout);

	/**
	 * The plot panel's text, one line per entry, empty when there is nothing to say.
	 *
	 * STATIC AND TAKING THE READOUT, for the same reason CommitPromptText is: the drawing
	 * needs a Canvas and the CONTENT does not, so the part that can go silently wrong stays
	 * testable with no world and no PIE.
	 */
	static TArray<FString> PanelLines(const FToolReadout& Readout);

private:
	/**
	 * The commit prompt, drawn AT THE CURSOR rather than on the bar.
	 *
	 * The bar already carries a Build button and it was not enough: it is the tenth control
	 * along in a group of six that all look alike, and reaching the last stage only un-greys
	 * it. The player's eyes are on the plot they are dragging, which is where the offer to
	 * build has to be - PIE, 2026-09-16, "i cant see the option to build".
	 */
	void DrawPlotPanel(const FVector2D& PlanePoint, const TArray<FString>& Lines);

	/**
	 * PanelLines' last answer, and the ARoadBuildController::GetToolReadoutRevision() it was
	 * built from - issue #190. DrawHUD calls PanelLines every frame it draws a tool's preview,
	 * which is a Printf per fact even on a frame the controller's own readout did not change;
	 * comparing the revision instead of rebuilding is what a still cursor now costs nothing for.
	 *
	 * -1 SENTINEL, not 0: GetToolReadoutRevision() starts at 0 too, and a fresh HUD comparing
	 * 0 == 0 on its first frame would serve an empty CachedPanelLines instead of ever building
	 * one until the revision moved past 0.
	 */
	TArray<FString> CachedPanelLines;
	int32 CachedPanelLinesRevision = -1;

	/**
	 * Resolved once in BeginPlay, not by DrawPlotPanel - issue #309: DrawPlotPanel called
	 * UAirportMgrUISettings::ResolveStyle() (a TSoftObjectPtr::LoadSynchronous) itself, every
	 * frame it drew a tool's preview panel, for a style that cannot change once resolved - the
	 * same shape #187 fixed on the panels, which this AHUD (not a UAirportMgrPanelWidget) had
	 * no base class to inherit that fix from. The ResolveStyle() fallback at the one call site
	 * below only covers a HUD asked to draw before BeginPlay has run, which a test constructing
	 * one directly and calling DrawPlotPanel without BeginPlay could still do.
	 */
	const UUIStyle* CachedStyle = nullptr;

	/** The controller this HUD belongs to, if it is the road build controller. */
	ARoadBuildController* GetBuildController() const;

	/**
	 * Node index text, gated on bDrawNodeIndices alone.
	 *
	 * The rings themselves come from GraphOverlay::Describe now - see DrawHUD - so this is
	 * only the label loop that used to live inside DrawNodes. Off by default; it clutters a
	 * dense graph.
	 */
	void DrawNodeIndices(const ARoadNetworkActor& Target);

	/**
	 * Anchor id text, gated on bDrawAnchorIds alone.
	 *
	 * The anchor rings themselves come from GraphOverlay::Describe now - see DrawHUD - so
	 * this is only the label loop that used to live inside DrawStands.
	 */
	void DrawAnchorIds(const ARoadNetworkActor& Target);

	/**
	 * A style's look, falling back to a checkNoEntry() backstop - same role StyleColour's
	 * switch used to play - if Looks is missing an entry the seeding loop should have added.
	 */
	const FPreviewLook& LookFor(EPreviewStyle Style) const;

public:
	/** LookFor, exposed for FRoadBuildHUDLooksTest - see #104. */
	const FPreviewLook& LookForTest(EPreviewStyle Style) const { return LookFor(Style); }

private:

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
