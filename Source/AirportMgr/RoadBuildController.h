#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"
#include "BuildCameraRig.h"
#include "GameFramework/PlayerController.h"
#include "RoadBuildLog.h"
#include "Tool/BuildGesture.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "Tool/Selection.h"
#include "RoadBuildController.generated.h"

class ARoadNetworkActor;
class UAircraftType;
class UOpsRuntime;
class UFlightBoard;
struct FAgentFacts;
struct FAirframe;
struct FStandFacts;
class UBuildCameraComponent;
class UBuildHudLayer;

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
 *
 * SPLIT by issue #94: this class carried seven concerns as roughly 40 UPROPERTYs and a .cpp
 * to match - the view rig, the watch rig, four widget classes, a testing override and
 * placement. The camera (both rigs, CreateBuildCamera, UpdateView, ZoomBy, ToggleWatchAgent's
 * mechanics) is now UBuildCameraComponent, a subobject; the four HUD widgets are
 * UBuildHudLayer, a subobject. What remains here is INPUT (binding keys, reading them, the
 * click/drag/release gesture), SESSION AND TARGET (which tool is active, which actor is
 * being built into), and forwarding - every public method BuildActions() or Blueprint could
 * already call keeps its name, whether the work happens here or in a subobject now.
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

	/**
	 * Furthest a click may place a node, as a MULTIPLE of the active camera's own distance.
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

	/**
	 * How far the mouse must move while held, in pixels, before a press on a node becomes a
	 * drag rather than a click.
	 *
	 * Without a threshold every slightly imprecise click on a node would nudge it, and the
	 * click-to-chain interaction would become impossible to perform reliably.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Move", meta = (ClampMin = "0.0"))
	double DragThresholdPixels = 4.0;

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

	/**
	 * Flip one guide source on the airport this controller drives. The bar buttons call this.
	 *
	 * THROUGH THE TARGET, because the settings live on the airport and not on the driver - see
	 * FSnapGuideSettings. A copy here would be a second place for them to drift, which is the
	 * failure FRoadSnapSettings already records.
	 */
	void ToggleGuideSource(SnapGuide::ESource Source);

	/** Whether that source is live. What lights the button. */
	bool IsGuideSourceOn(SnapGuide::ESource Source) const;

	/** The tool the number keys selected, or null before BeginPlay has built them. */
	IBuildTool* GetActiveTool() const;

	/** Everything the active tool needs to judge the current cursor. */
	FToolContext MakeToolContext() const;

	/**
	 * What the active tool says about the gesture in progress - bays, warnings, whether
	 * Build would succeed. Refilled every PlayerTick; empty when no tool is speaking.
	 *
	 * COLLECTED HERE AND NOT IN THE HUD, though the HUD is where BuildPreview is called
	 * from: a readout is not a drawing concern, and the bar must be able to read it whether
	 * or not the world preview is switched on (see bDrawBuildPreview).
	 */
	const FToolReadout& GetToolReadout() const { return ToolReadoutCollector.Readout; }

	/** Drives one frame's collection without a whole tick. Same precedent as GetHudForTest. */
	void CollectToolReadoutForTest() { CollectToolReadout(); }

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

	/** Forwards to the camera component - see UBuildCameraComponent::IsWatchingAgent. */
	bool IsWatchingAgent() const;
	bool IsGuidelineOverlayOn() const { return bShowGuidelines; }
	bool CanUndo() const;
	bool CanRedo() const;
	bool HasNetworkContent() const;
	bool HasRunway() const;
	bool HasAgent() const;
	bool HasOpsRuntime() const;

	/**
	 * Move the landing fee one step, up or down. See UPricing::LandingFeeMultiplier.
	 *
	 * STEPS RATHER THAN A FREE SLIDER, for the reason ESimSpeed is an enum and not a float:
	 * the game offers these settings, and two code paths cannot then disagree about what
	 * "higher" means. Clamped at both ends - a zero fee would make DemandFactor meaningless
	 * and a tenfold one would empty the inbox with no way back that reads as a mistake.
	 */
	void StepLandingFee(int32 Delta);

	/** Open or close the ledger panel. The game.ledger action's verb. */
	void ToggleLedger();

	/** Whether the ledger panel is open, so the bar's button can light itself. */
	bool IsLedgerShowing() const;
	bool IsPaused() const;

	void StepSpeed(int32 Delta);
	void TogglePause();
	void QuickSave();
	void QuickLoad();

	/** Mouse wheel. Forwards to the camera component. Public, like the other bar/key
	 *  actions above - SetupInputComponent binds these the same way it binds those. */
	void ZoomIn();
	void ZoomOut();

	/**
	 * Lands at the runway nearest the VIEW FOCUS. The bar's Land button is clicked with the
	 * cursor on the bar, where "nearest the cursor" is meaningless; the focus is where the
	 * player is looking. The key does the same, for one-action-one-behaviour.
	 *
	 * NOT a tool, and not in ToolRegistry(): an arrival is one decision taken at the view
	 * focus rather than a gesture with states, so giving it an IBuildTool would be inventing
	 * a mode for it to sit in.
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

	/**
	 * Build: the active tool commits whatever it has been drawing.
	 *
	 * A VERB ON THE CONTROLLER rather than the bar reaching into the tool, for the same
	 * reason every other entry in BuildActions() is: the registry generates the key bindings
	 * and the buttons from one list, and an action whose Execute knew about IBuildTool would
	 * be the only one that did.
	 */
	void OnBuild();

	/**
	 * C: orbit the SELECTED aircraft, or the newest when none is selected; or go back to the
	 * build view.
	 *
	 * The PREFERENCE (selected, else newest) is Session/Selection policy and stays here; the
	 * mechanics of riding the aircraft are UBuildCameraComponent::ToggleWatchAgent's - see
	 * that class's own comment for why the split falls there.
	 */
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

	/** The widget layer this instance owns, or null before construction has run. For a test
	 *  that CreateDefaultSubobject was not dropped - same precedent as SessionForTest,
	 *  GestureForTest and ResolveProfileForTest. The camera component needs no equivalent:
	 *  it is an actual UActorComponent, so FindComponentByClass already answers that. */
	UBuildHudLayer* GetHudForTest() const { return Hud; }

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

	/**
	 * One frame's readout from the active tool.
	 *
	 * RESETS UNCONDITIONALLY, before any guard: a frame with no target or no tool must leave
	 * the readout EMPTY rather than holding the last gesture's facts on the bar. Called from
	 * PlayerTick ahead of the target guard for exactly that reason.
	 */
	void CollectToolReadout();

	void OnCancelGesture();

	/**
	 * The registry's key handler: finds the action whose key and Ctrl requirement match the
	 * chord that fired. One handler for every key, so a binding cannot exist without an
	 * action behind it.
	 *
	 * ONE handler bound once per registry entry, rather than one dedicated method per tool
	 * (issue #33 removed six of those - a "select the Nth tool" method for each key) or a
	 * lambda per BindKey call: the registry already carries the FKey, so a handler that
	 * receives it back needs no capture and no second place to say which index goes with
	 * which key. Any key not in the registry (there is none, by construction) is silently
	 * ignored.
	 */
	void OnActionKey(FKey Key);

	/** Chord bindings carry no key, so Ctrl actions share this and ask which key was just pressed. */
	void OnCtrlActionKey();

	UPROPERTY(Transient) EClickModifier ClickModifier = EClickModifier::None;

	/** True while Ctrl is held: the gesture means remove rather than build. */
	bool IsRemoveHeld() const;

	/** The agent whose projected position is nearest the cursor within AgentPickPixels, or 0. */
	int32 HoverAgentUnderCursor() const;

	/** Read WASD/QE/wheel/mouse-drag into axes and hand them to the camera component; owns
	 *  the raw reads because that is host input, not camera geometry. */
	void UpdateView(float DeltaTime);

	/**
	 * Horizontal pixels the cursor has moved since last frame WHILE THE DRAG BUTTON IS
	 * HELD, and zero otherwise. Advances the drag state as a side effect, so it is called
	 * exactly once per frame, from UpdateView.
	 *
	 * Differencing the cursor POSITION rather than reading GetInputMouseDelta: that reports
	 * the delta of a CAPTURED mouse, and a builder runs with a visible, uncaptured cursor
	 * because the player has to be able to click a bar button. Differencing the position
	 * works either way, and it is what the tools already use to hit-test the world.
	 *
	 * HORIZONTAL ONLY. Vertical drag does nothing on purpose - pitch is not stored on the
	 * rig, it is a function of distance (see FBuildCameraRig), so there is no pitch for a
	 * drag to change. Tilting would have to become a second, independent axis and would put
	 * the camera in poses the zoom could not reproduce.
	 */
	double ReadMouseTurnPixels();

	/** Cursor position last frame, for ReadMouseTurnPixels. Meaningless unless
	 *  bRotatingWithMouse. */
	FVector2D LastMousePosition = FVector2D::ZeroVector;

	/** Was the drag button held LAST frame? The first frame of a drag has nothing to
	 *  difference against and must contribute nothing - without this the view jumps by
	 *  however far the cursor moved since the button was last released. */
	bool bRotatingWithMouse = false;

	/**
	 * The camera: both rigs, the spawned ACameraActor, CreateBuildCamera/UpdateView/ZoomBy
	 * and the mechanics of ToggleWatchAgent - see UBuildCameraComponent's own comment.
	 * CreateDefaultSubobject rather than NewObject: this is an ActorComponent, and
	 * CreateDefaultSubobject is what REGISTERS it with the owning actor - the requirement
	 * for it to participate in save/duplicate at all, the same reasoning
	 * ARoadNetworkActor's own subobjects follow. It does NOT tick
	 * (PrimaryComponentTick.bCanEverTick is false in the constructor): UpdateView is called
	 * explicitly from PlayerTick instead, in the same frame as input is read - see that
	 * constructor's own comment for why a component tick would run at the wrong point.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside")
	TObjectPtr<UBuildCameraComponent> BuildCameraComp;

	/**
	 * The four HUD widgets and their configured classes - see UBuildHudLayer's own comment.
	 * A UObject, not a component: it owns no transform and ticks nothing, so it costs
	 * nothing more than a UPROPERTY pointer to hold it. CreateDefaultSubobject rather than
	 * NewObject - the same call as BuildCameraComp's above works for any UObject subobject,
	 * component or not; see ARoadNetworkActor::Facade (URoadEditFacade) for the same plain-
	 * UObject use of it already established in this codebase.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside")
	TObjectPtr<UBuildHudLayer> Hud;

	/** Resolved once on BeginPlay; the first ARoadNetworkActor in the level. */
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/**
	 * The tools, which one is active, and the snap/placement rules a click is judged
	 * against - see FBuildSession. Owned here rather than as separate fields so the editor
	 * mode's URoadBuildEditorTool can hold the identical state and the two cannot drift the
	 * way they did before issue #33.
	 */
	FBuildSession Session;

	/**
	 * This frame's readout, refilled by CollectToolReadout.
	 *
	 * NOT A UPROPERTY: FToolReadoutCollector is a plain struct holding FStrings, with no
	 * UObject reference in it, so there is nothing here for the GC to keep alive.
	 */
	FToolReadoutCollector ToolReadoutCollector;

	// --- Press, drag, release ---------------------------------------------------------
	//
	// The controller decides what the MOUSE did - a click or a drag, and how far a press
	// must travel to count as one. What that MEANS is the tool's business, and a tool
	// never sees a raw key. The recogniser itself is FBuildGesture (Tool/BuildGesture.h),
	// shared with URoadBuildEditorTool - see issue #92; this class keeps only the host I/O
	// of reading the mouse and calling into IBuildTool.

	FBuildGesture Gesture;
};
