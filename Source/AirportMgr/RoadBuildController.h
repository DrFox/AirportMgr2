#pragma once

#include "CoreMinimal.h"
#include "BuildCameraRig.h"
#include "GameFramework/PlayerController.h"
#include "Tool/BuildGesture.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "Tool/Selection.h"
#include "RoadBuildController.generated.h"

class ARoadNetworkActor;
class UBuildBarWidget;
class UAircraftType;
class UInspectorWidget;
class UOfferInboxWidget;
class UToastStackWidget;
class UOpsRuntime;
class UFlightBoard;
struct FAgentFacts;
struct FAirframe;
struct FStandFacts;

/**
 * What a plain click means right now. ONE ENUM: Remove and Insert can never both be lit,
 * so the state that would need a rule to resolve is not representable (CLAUDE.md, "a phase
 * is an enum, never a set of bools"). Ctrl and Shift held on the keyboard OR with this in
 * MakeToolContext, so the keys keep working and light the same button.
 */
UENUM()
enum class EClickModifier : uint8
{
	None,
	Remove,
	Insert
};

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
 * It lives in the game module rather than the Airside plugin because a PlayerController
 * is game-framework glue. The plugin must not depend on the game.
 */

UCLASS(Config = Game)
class AIRPORTMGR_API ARoadBuildController : public APlayerController
{
	GENERATED_BODY()

public:
	ARoadBuildController();

	/**
	 * How close, in uu, the cursor counts as "on" something a tool is asking about - a
	 * guideline node to route from, a stand to pick up, an apron's first corner.
	 *
	 * A VIEW FACT, not an airport one - see ARoadNetworkActor::Snap for the road-snap radii
	 * this used to sit beside (moved there by issue #93, now that both drivers judge a click
	 * by the same per-airport rules). This one stays here: it is the runtime driver's own
	 * answer to "what is the cursor pointing at" - the editor tool asks the same question
	 * from ARoadNetworkActor::MakeTunables' view-scaled default instead, since it has no
	 * fixed view distance of its own to size a constant from.
	 *
	 * SEPARATE from the road-snap radius, and larger. The two answer different questions and
	 * only ever looked like one number by coincidence: the snap radius decides where a road
	 * NODE goes, and wants to be tight or roads land where you did not click. This decides
	 * what the cursor is POINTING AT, and 150 uu is a punishing target - a guideline node is
	 * a dimensionless point on a road 200 uu wide, viewed from 8000 uu out, so the route tool
	 * read as doing nothing at all when it was simply being missed.
	 *
	 * Road snapping no longer depends on this number anyway: a junction claims the cursor out
	 * to its own pavement (FRoadSnapSettings::JunctionSnapFactor), so widening the fixed
	 * radius here would only have made BARE nodes grabbier for no gain.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.0"))
	double ToolPickRadius = 400.0;

	/**
	 * How close, in PIXELS, the cursor must be to an aircraft's projected position to pick
	 * it. Pixels, not uu: an aircraft on final is clicked in screen space (spec §3.3), and a
	 * radius that shrank with distance would make the far ones unclickable.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1.0"))
	double AgentPickPixels = 24.0;

	/**
	 * Draw the guideline graph - the routes agents follow - under every tool. Toggled by G.
	 *
	 * Defaults ON. It used to be drawn only while the route tool was selected, so the graph
	 * you are building FOR was invisible while you built it, and a defect at the
	 * road/guideline boundary stayed hidden until someone happened to press 4.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View")
	bool bShowGuidelines = true;

	/**
	 * The bar's Blueprint class. Config so DefaultGame.ini names WBP_BuildBar without a
	 * Blueprint subclass of this controller existing to hold the default. Null means the
	 * plain C++ bar, which works and says so in the log.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UBuildBarWidget> BuildBarClass;

	/** The bar on screen, created at BeginPlay. */
	UPROPERTY(Transient) TObjectPtr<UBuildBarWidget> BuildBar;

	/** The inspector's Blueprint class; null means the plain C++ panel. Config, like the bar's. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UInspectorWidget> InspectorClass;

	/** The inspector on screen, created at BeginPlay beside the bar. */
	UPROPERTY(Transient) TObjectPtr<UInspectorWidget> Inspector;

	/** The offer inbox's Blueprint class; null means the plain C++ panel, as above. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UOfferInboxWidget> OfferInboxClass;

	/** The inbox on screen. Play-mode only: the editor mode has no runtime to read. */
	UPROPERTY(Transient) TObjectPtr<UOfferInboxWidget> OfferInbox;

	/**
	 * What key 7 lands. Null - the shipping state - lands the content set's default.
	 *
	 * A TESTING OVERRIDE, and deliberately shaped so it cannot quietly become the game's
	 * behaviour: it is consulted by the Land key and by nothing else, so offers, dispatch and
	 * every arrival that comes from the flight board still resolve their own type. What it
	 * buys is not having to wait for an offer to see a particular aeroplane on the runway.
	 *
	 * CONFIG, so setting it is one line in DefaultGame.ini and unsetting it is deleting that
	 * line - no rebuild either way. The log says which type the key used every time, so a
	 * forgotten override reads as a line in the log rather than as the wrong aircraft
	 * mysteriously landing.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|Testing")
	TSoftObjectPtr<UAircraftType> LandAircraftType;

	/** The toast stack's Blueprint class; null means the plain C++ stack, as above. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UToastStackWidget> ToastStackClass;

	/** The feed on screen. Owns the notification centre; see UToastStackWidget. */
	UPROPERTY(Transient) TObjectPtr<UToastStackWidget> ToastStack;

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
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0"))
	double MaxPlaceDistanceFactor = 6.0;

	/**
	 * Use the orbiting build camera instead of the pawn's own view.
	 *
	 * Viewing through a camera actor also takes the view away from the pawn, so the pawn's
	 * mouse-look stops fighting the cursor for the same input.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View")
	bool bStartAbovePlane = true;

	/** Camera-to-focus distance the session opens at, in uu. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0"))
	double StartViewDistance = 8000.0;

	/** Closest the camera may come, in uu. Sized to sit beside a vehicle. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0"))
	double MinViewDistance = 600.0;

	/** Furthest the camera may pull back, in uu. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0"))
	double MaxViewDistance = 60000.0;

	/** Pitch at MinViewDistance, in degrees below horizontal. Near eye level. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MinPitchDegrees = 30.0;

	/**
	 * Pitch at MaxViewDistance. 90 would be straight down, and is deliberately not
	 * offered: control rotation renormalises unpredictably at the poles, and a view that
	 * flat loses every cue about relief that the angle exists to provide.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MaxPitchDegrees = 70.0;

	/** Fraction the view distance changes per mouse-wheel notch. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.01", ClampMax = "0.9"))
	double ZoomStep = 0.15;

	/**
	 * Pan speed, in view distances per second.
	 *
	 * Not uu per second: the view spans a hundredfold range, and a fixed speed crawls when
	 * zoomed out and overshoots when zoomed in. As a fraction of the view, a pan crosses
	 * the same amount of screen at every zoom.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double PanRate = 0.9;

	/**
	 * How far the mouse must move while held, in pixels, before a press on a node becomes a
	 * drag rather than a click.
	 *
	 * Without a threshold every slightly imprecise click on a node would nudge it, and the
	 * click-to-chain interaction would become impossible to perform reliably.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Move", meta = (ClampMin = "0.0"))
	double DragThresholdPixels = 4.0;

	/** Rotation speed on Q and E, in degrees per second. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double RotateRate = 90.0;

	/** Seconds the view takes to settle after an input. Zero snaps. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double CameraLag = 0.12;

	/** Horizontal field of view, in degrees. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "20.0", ClampMax = "150.0"))
	double FieldOfView = 75.0;

	/**
	 * Show the ghost of the segment the next click would build.
	 *
	 * Real solved pavement on a duplicate of the graph, not a rubber band: it carries the
	 * road's actual width and the shape the junction at either end will take.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	bool bDrawBuildPreview = true;

	// --- Read side, for ARoadBuildHUD ------------------------------------------------
	//
	// The overlay draws what this controller has decided; it must not decide anything
	// itself. Exposing the decisions rather than the state is what keeps the two from
	// drifting into disagreeing about which node the next click will use.

	/** The road actor being built into, or null when the level has none. */
	ARoadNetworkActor* GetTarget() const { return Target; }

	/** The tool the number keys selected, or null before BeginPlay has built them. */
	IBuildTool* GetActiveTool() const;

	/** Everything the active tool needs to judge the current cursor. */
	FToolContext MakeToolContext() const;

	// --- Actions ---------------------------------------------------------------------
	//
	// Everything below is what BuildActions() calls. The registry, not this class, decides
	// which key and which button each maps to; these are the verbs and the state queries.

	/** Selects a tool by registry index and clears the click modifier - see EClickModifier. */
	void SelectTool(int32 Index);
	int32 GetActiveToolIndex() const;

	/** Lights Mode, or clears it if it was already lit. */
	void ToggleClickModifier(EClickModifier Mode);
	EClickModifier GetClickModifier() const { return ClickModifier; }

	bool IsWatchingAgent() const { return bWatchingAgent; }
	bool IsGuidelineOverlayOn() const { return bShowGuidelines; }
	bool CanUndo() const;
	bool CanRedo() const;
	bool HasNetworkContent() const;
	bool HasRunway() const;
	bool HasAgent() const;
	bool HasOpsRuntime() const;
	bool IsPaused() const;

	void StepSpeed(int32 Delta);
	void TogglePause();
	void QuickSave();
	void QuickLoad();

	/**
	 * Lands at the runway nearest the VIEW FOCUS. The bar's Land button is clicked with the
	 * cursor on the bar, where "nearest the cursor" is meaningless; the focus is where the
	 * player is looking. The key does the same, for one-action-one-behaviour.
	 */
	void LandAircraftNearViewFocus();

	/**
	 * The land key's flight-board path: one flight with an immediate ETA, accepted at once.
	 *
	 * Split out rather than inlined so that the no-runtime fallback above it stays legible -
	 * the editor mode has no game instance, and so no board, and the key must still work
	 * there. Logs the refusal sentence when the airport cannot take it.
	 */
	void LandThroughTheBoard(UOpsRuntime& Runtime, UFlightBoard& Board, const FAirframe& Airframe);

	void OnClearNetwork();
	void OnUndo();
	void OnRedo();

	/** C: orbit the SELECTED aircraft, or the newest when none is selected; or go back to
	 *  the build view. */
	void ToggleWatchAgent();

	// --- Selection (the inspector's verbs) --------------------------------------------
	const FSelection& GetSelection() const { return Session.GetSelection(); }
	bool HasSelectedAircraft() const { return GetSelection().Kind == ESelectionKind::Aircraft; }
	/** The selected aircraft's facts, or false when nothing is selected or it has gone. */
	bool SelectedAgentFacts(FAgentFacts& Out) const;
	bool SelectedStandFacts(FStandFacts& Out) const;
	bool CanDepartSelected() const;
	/** Depart the selected aircraft; logs the planner's answer. */
	void DepartSelected();

	/**
	 * What the next click would do, run through the snap chain. False only when the
	 * cursor is not over the road plane at all.
	 *
	 * The single source of truth for the decision: the overlay draws this and the active
	 * tool acts on it, so what is highlighted and what happens cannot disagree.
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

	/** G: show or hide the guideline overlay. */
	void OnToggleGuidelines();

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

private:
	/** Left button down: remember where. Decides nothing - that waits for the release. */
	void OnPrimaryPressed();

	/** Left button up: a drag ends, or - if it never became one - it was a click. */
	void OnPrimaryReleased();

	/** Promote a held press to a drag once it has travelled, and feed the tool. */
	void UpdateDrag();

	void OnCancelGesture();

	/**
	 * The registry's key handler: finds the action whose key and Ctrl requirement match the
	 * chord that fired. One handler for every key, so a binding cannot exist without an
	 * action behind it.
	 */
	void OnActionKey(FKey Key);

	/** Chord bindings carry no key, so Ctrl actions share this and ask which key was just pressed. */
	void OnCtrlActionKey();

	UPROPERTY(Transient) EClickModifier ClickModifier = EClickModifier::None;

	/** True while Ctrl is held: the gesture means remove rather than build. */
	bool IsRemoveHeld() const;

	/**
	 * The key that was pressed, looked up against ToolRegistry() to find which tool it
	 * selects.
	 *
	 * ONE handler bound once per registry entry, rather than one dedicated method per tool
	 * (issue #33 removed six of those - a "select the Nth tool" method for each key) or a
	 * lambda per BindKey call: the registry already carries the FKey, so a handler that
	 * receives it back needs no capture and no second place to say which index goes with
	 * which key. Any key not in the registry (there is none, by construction) is silently
	 * ignored.
	 */
	void SelectToolByKey(FKey Key);

	// --- Watch camera -----------------------------------------------------------------
	//
	// A second camera MODE rather than a second camera: the build rig is a top-down thing
	// for laying pavement, and watching a take-off from it shows a dot getting smaller. This
	// orbits the aircraft instead, and hands back the moment there is nothing to watch.
	//
	// It is the SAME rig type as the build view, kept in the aircraft's frame (see
	// FBuildCameraRig::InFrame), so the wheel, WASD and Q/E do in watch mode exactly what
	// they do while building: zoom, slide the look-at point, orbit. Two rigs rather than
	// one re-aimed, so leaving watch mode lands on the build view where it was left.

	/** True while the camera is riding with an agent. */
	bool bWatchingAgent = false;

	/** The agent the watch camera rides: the selected one at toggle time, else the newest. */
	int32 WatchAgentId = 0;

	/** The agent whose projected position is nearest the cursor within AgentPickPixels, or 0. */
	int32 HoverAgentUnderCursor() const;

	/** Where the watch rig is asked to be, and where it is; relative to the aircraft. */
	FBuildCameraRig WatchTarget;
	FBuildCameraRig WatchCurrent;

	/** Copy the watch tunables below onto a rig. The watch twin of ApplyViewLimits. */
	void ApplyWatchLimits(FBuildCameraRig& Rig) const;

	/**
	 * Camera-to-aircraft distance on pressing C, uu. 1550 is 15.5 m.
	 *
	 * Wide enough to frame a 13 m wingspan and close enough to read the attitude, which is
	 * the whole point of watching a rotation.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "1.0"))
	double WatchStartDistance = 1550.0;

	/**
	 * Direction the camera looks on pressing C, degrees from the aircraft's heading.
	 *
	 * -75 looks across the aircraft from off its right wing and a little behind, which is
	 * the pose the fixed watch camera had: side-on enough to read pitch, angled enough
	 * to see the nose.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch")
	double WatchStartYaw = -75.0;

	/** Height of the look-at point above the aircraft's origin, uu. Its origin is the
	 *  main-gear axle; 150 is about the fuselage centreline. */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch")
	double WatchFocusHeight = 150.0;

	/** Closest the wheel may bring the camera to the aircraft, uu. */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "1.0"))
	double WatchMinDistance = 800.0;

	/** Furthest the wheel may pull back while still following, uu. */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "1.0"))
	double WatchMaxDistance = 20000.0;

	/**
	 * Pitch at WatchMinDistance, degrees below horizontal.
	 *
	 * Lower than the build rig's: the build rig's floor keeps the road plane readable,
	 * while this one wants to be near eye level beside an aircraft.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double WatchMinPitchDegrees = 10.0;

	/** Pitch at WatchMaxDistance, degrees below horizontal. */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double WatchMaxPitchDegrees = 60.0;

	/**
	 * Furthest WASD may slide the look-at point from the aircraft, uu.
	 *
	 * Without a leash, W held for a few seconds carries the focus off into the grass and
	 * the aircraft leaves the frame, with nothing on screen to say which way it went.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "0.0"))
	double WatchMaxFocusOffset = 2000.0;

	/**
	 * Lands an aircraft on the runway nearest the cursor and taxis it to a stand. Key 7.
	 *
	 * NOT a tool, and not in ToolRegistry(): an arrival is one decision taken at the cursor
	 * rather than a gesture with states, so giving it an IBuildTool would be inventing a
	 * mode for it to sit in.
	 */
	void OnLandAircraft();

	/** World-space position of a node, at the road plane's height. */
	bool NodeWorldLocation(int32 NodeIndex, FVector& OutLocation) const;

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

	/**
	 * The tools, which one is active, and the snap/placement rules a click is judged
	 * against - see FBuildSession. Owned here rather than as separate fields so the editor
	 * mode's URoadBuildEditorTool can hold the identical state and the two cannot drift the
	 * way they did before issue #33.
	 */
	FBuildSession Session;

	// --- Press, drag, release ---------------------------------------------------------
	//
	// The controller decides what the MOUSE did - a click or a drag, and how far a press
	// must travel to count as one. What that MEANS is the tool's business, and a tool
	// never sees a raw key. The recogniser itself is FBuildGesture (Tool/BuildGesture.h),
	// shared with URoadBuildEditorTool - see issue #92; this class keeps only the host I/O
	// of reading the mouse and calling into IBuildTool.

	FBuildGesture Gesture;
};
