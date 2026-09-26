#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameValue.h"
#include "Model/InspectFacts.h"
#include "Solve/GuideArbiter.h"
#include "BuildCameraRig.h"
#include "GameFramework/PlayerController.h"
#include "RoadBuildLog.h"
#include "Solve/RoadGeom.h"
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
struct FAirframe;
struct FStandFacts;
class UBuildCameraComponent;
class UBuildHudLayer;
class URoadEditFacade;


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
 * to match - the view rig, the watch rig, four widget classes (a fifth, the ledger panel,
 * joined later), a testing override and placement. The camera (both rigs, CreateBuildCamera,
 * UpdateView, ZoomBy, ToggleWatchAgent's mechanics) is now UBuildCameraComponent, a subobject;
 * the five HUD widgets are UBuildHudLayer, a subobject. What remains here is INPUT (binding
 * keys, reading them, the click/drag/release gesture), SESSION AND TARGET (which tool is
 * active, which actor is being built into), and forwarding - every public method
 * BuildActions() or Blueprint could already call keeps its name, whether the work happens
 * here or in a subobject now.
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
	 * What key 7 lands. Null lands the content set's default; DefaultGame.ini currently sets
	 * this to a specific test type (issue #191 fixed this comment - it used to call null "the
	 * shipping state", which stopped being true the moment a project .ini line set it).
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
	 *
	 * DEFAULTED FROM RoadGeom::DefaultMaxPlaceDistanceFactor, not a second 6.0 (issue
	 * #191/#92-#93): the editor tool used to apply no cap at all
	 * (TNumericLimits<double>::Max() in URoadBuildEditorTool::RayToPlane) while this one
	 * guarded the horizon, so the same near-horizon click behaved differently depending on
	 * which driver was open. The editor now measures its own view-centre distance
	 * (URoadBuildEditorTool::ViewCentreDistance, set from Render) and applies the same
	 * shared factor - see that class's own comment.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "1.0"))
	double MaxPlaceDistanceFactor = RoadGeom::DefaultMaxPlaceDistanceFactor;

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
	 *
	 * DEFAULTED FROM FBuildGesture::DefaultThresholdPixels, not a second 4.0 (issue
	 * #191/#92-#93) - see that constant's own comment for the third copy this replaced.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Move", meta = (ClampMin = "0.0"))
	double DragThresholdPixels = FBuildGesture::DefaultThresholdPixels;

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
	void ToggleGuideRelation(SnapGuide::ERelation Relation);

	/** Whether that row is lit. */
	bool IsGuideRelationOn(SnapGuide::ERelation Relation) const;

	/** Flips one COLUMN of the guide grid - what a SNAP TO button does. Same ownership rule. */
	void ToggleGuideReference(SnapGuide::EReference Reference);

	/** Whether that column is lit. */
	bool IsGuideReferenceOn(SnapGuide::EReference Reference) const;

	/** The tool the number keys selected, or null before BeginPlay has built them. */
	IBuildTool* GetActiveTool() const;

	/**
	 * Everything the active tool needs to judge the current cursor, built FRESH: runs the
	 * whole snap + guide pipeline (FBuildSession::MakeContext - a junction solve per live
	 * node outside its cheap-reject radius, then the guide chain) every time it is called.
	 *
	 * CALL THIS SPARINGLY. It exists for the handful of call sites that genuinely need a
	 * context newer than the frame's own - a click or drag event reading the mouse position
	 * at the moment it fired, not at the top of PlayerTick - see GetFrameContext() for the
	 * one built once a frame and shared by everything that does not. Issue #167: before the
	 * split, EVERY reader called this directly, 3-5 times in a single PlayerTick.
	 */
	FToolContext MakeToolContext() const;

	/**
	 * This frame's context, built ONCE at the top of PlayerTick and read by CollectToolReadout,
	 * Tick, and ARoadBuildHUD::DrawHUD (which runs after this frame's PlayerTick, over the
	 * same mouse position) - issue #167. Before this, each of those three called
	 * MakeToolContext() independently, so a frame with no drag paid for the whole snap + guide
	 * pipeline three times over for one cursor position; a drag frame paid for it five.
	 *
	 * NOT rebuilt for UpdateDrag: a drag reads the mouse position again, after this was built,
	 * so it calls MakeToolContext() itself for a context that reflects where the cursor
	 * actually is right now - see UpdateDrag's own comment.
	 */
	const FToolContext& GetFrameContext() const { return FrameContext; }

	/**
	 * What the active tool says about the gesture in progress - bays, warnings, whether
	 * Build would succeed. Refilled every PlayerTick; empty when no tool is speaking.
	 *
	 * COLLECTED HERE AND NOT IN THE HUD, though the HUD is where BuildPreview is called
	 * from: a readout is not a drawing concern, and the bar must be able to read it whether
	 * or not the world preview is switched on (see bDrawBuildPreview).
	 */
	const FToolReadout& GetToolReadout() const { return ToolReadoutCollector.Readout; }

	/** Drives one frame's collection without a whole tick. Same precedent as GetHudForTest.
	 *  Refreshes FrameContext first - issue #167 made CollectToolReadout read that member
	 *  rather than build its own, and this must still work standalone, with no PlayerTick
	 *  to have built it. */
	void CollectToolReadoutForTest() { FrameContext = MakeToolContext(); CollectToolReadout(); }

	/**
	 * How many times CollectToolReadout has actually rebuilt ToolReadoutCollector, rather than
	 * reusing last frame's answer - see FToolReadoutKey. ARoadBuildHUD::DrawHUD caches
	 * PanelLines' TArray<FString> against this number instead of rebuilding it (a Printf per
	 * fact) every frame, and issue #190's composition test reads it directly: K ticks with a
	 * still cursor must advance it once, not K times.
	 */
	int32 GetToolReadoutRevision() const { return ToolReadoutRevision; }

	/** Runs PlayerTick without a real tick loop - same precedent as CollectToolReadoutForTest.
	 *  Issue #167's composition test uses this to prove PlayerTick builds exactly one context
	 *  and hands it to CollectToolReadout and Tick, rather than each building its own. The
	 *  caller must have called InitInputSystem() first - PlayerTick asserts a PlayerInput
	 *  exists, which a bare SpawnActor never creates on its own. */
	void PlayerTickForTest(float DeltaTime) { PlayerTick(DeltaTime); }

	/** Points this instance at InTarget without going through BeginPlay's level search - same
	 *  precedent as PlayerTickForTest, for a test that has no level to search. */
	void SetTargetForTest(ARoadNetworkActor* InTarget) { Target = InTarget; BindRunwayCacheInvalidation(); }

	/** How many FToolContexts FBuildSession has actually built, for the composition test
	 *  above: PlayerTickForTest brackets a tick with this to count contexts built DURING it,
	 *  rather than asserting on a side effect that would still look right if two contexts
	 *  happened to agree. See FBuildSession::MakeContextCallCountForTest. */
	int32 MakeContextCallCountForTest() const;

	// --- Actions ---------------------------------------------------------------------
	//
	// Everything below is what BuildActions() calls. The registry, not this class, decides
	// which key and which button each maps to; these are the verbs and the state queries.

	/** Selects a tool by registry index. The session drops a sticky build modifier with
	 *  it - see FBuildSession::SelectTool. */
	void SelectTool(int32 Index);
	int32 GetActiveToolIndex() const;

	/** Drives RunActionForKey with a SYNTHETIC Ctrl flag, same precedent as PlayerTickForTest:
	 *  a headless test has no viewport to hold a real key down, so OnActionKey's own
	 *  IsInputKeyDown read is bypassed rather than faked. See RunActionForKey's own comment
	 *  for the fallback this exists to prove (issue #192). */
	void OnActionKeyForTest(FKey Key, bool bCtrl) { RunActionForKey(Key, bCtrl); }

	/**
	 * Light Mode, or go back to Build if it was already lit. Forwards to the session, which
	 * owns the one mode so PIE and the editor mode cannot disagree about it - and so Remove,
	 * Insert and Edit cannot be lit at once, which they could while Edit was a second field
	 * on this class (reported from play, 2026-09-20; see EGestureMode).
	 *
	 * A TOGGLE AND NOT A HELD KEY for Edit specifically, ruled from play: a gesture that
	 * reshapes placed geometry has to be entered on purpose.
	 */
	void ToggleGestureMode(EGestureMode Mode);
	EGestureMode GetGestureMode() const;

	/**
	 * Runs Verb.Apply against this controller's own session and a freshly-built context, then
	 * invalidates the readout cache exactly as ToggleGestureMode above already does - issue
	 * #304. BuildActions.cpp's generated Remove/Insert/Edit rows call this rather than each
	 * growing its own controller forwarder the way ToggleGestureMode did, which is what lets a
	 * fourth entry in BuildVerbRegistry() need no new method here at all.
	 *
	 * ToggleGestureMode(EGestureMode) KEEPS ITS OLD NAME AND CALLERS regardless - ClickModifierTest.cpp
	 * calls it directly and continues to, per CLAUDE.md's refactor contract ("every reachable
	 * entry point stays reachable at its old name").
	 */
	void ApplyVerb(const FBuildVerbRegistration& Verb);

	/** The shared session, for BuildVerbRegistry()'s IsActive/IsEnabled predicates - both of
	 *  which read nothing else. */
	const FBuildSession& GetSession() const { return Session; }

	/** Whether the committed graph's node rings belong on screen - see
	 *  FBuildSession::WantsRoadNodesDrawn. Read by ARoadBuildHUD every frame. */
	bool WantsRoadNodesDrawn() const;

	/** Whether the LIT tool exposes anything to edit - what greys the Edit button out, so
	 *  the bar answers "why can I not edit this" rather than lighting over a mode that would
	 *  do nothing. Reads FToolRegistration::EditHandles, the one list. */
	bool ActiveToolHasEditHandles() const;

	/** Forwards to the camera component - see UBuildCameraComponent::IsWatchingAgent. */
	bool IsWatchingAgent() const;
	bool IsGuidelineOverlayOn() const { return bShowGuidelines; }
	bool CanUndo() const;
	bool CanRedo() const;
	bool HasNetworkContent() const;
	bool HasRunway() const;
	bool HasAgent() const;
	bool HasOpsRuntime() const;

	/** How many times HasRunway has actually re-walked the network (AirsideCapability::
	 *  Summarise), as opposed to how many times it was asked - the seam that measures the
	 *  cache below is doing something, the same idiom as UAirportMgrUISettings::
	 *  ResolveCallCountForTest. Read as a DELTA across calls, not an absolute count. */
	int32 HasRunwayRecomputeCountForTest() const { return RunwayRecomputeCountForTest; }

	// StepLandingFee(int32) WAS HERE, and was REMOVED, not forwarded, by issue #191: the
	// game.feedown/feeup actions used to reach it purely to get from a controller reference to
	// UOpsRuntime::GetPricing(). FBuildActionContext (BuildActions.h) now hands them Runtime
	// directly, so those actions call UPricing::StepLandingFee themselves and this method had
	// no remaining caller to forward for - see FBuildActionContext's own comment on why a verb
	// no longer has to become a controller method just to reach the object that actually owns
	// it. Not a UFUNCTION, so nothing outside this file could have named it either - see the
	// refactor contract's "every UFUNCTION and interface virtual stays reachable" clause,
	// which this removal does not fall under.

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
	 *
	 * A THIN FORWARDER as of issue #191: resolving which airframe lands and driving it through
	 * the flight board are now UOpsRuntime::LandNear's job (Present/ of AirportOps, which
	 * already owns the board) - this supplies the view focus, the configured test override if
	 * any, and the one path LandNear cannot cover: the editor mode's no-runtime direct dispatch.
	 */
	void LandAircraftNearViewFocus();

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

	/**
	 * The same facts as SelectedAgentFacts, computed AT MOST ONCE PER FRAME regardless of how
	 * many callers ask - issue #187. Before this, the bar's selection.depart row
	 * (CanDepartSelected, below) and UInspectorWidget::Refresh each called
	 * InspectFacts::DescribeAgent independently, every tick, for the one selected aircraft -
	 * three FString allocations apiece, twice over, for facts that cannot have changed between
	 * the two calls in the same frame. TFrameValue rather than a hand-rolled GFrameCounter
	 * check: the engine already has exactly this cache-for-one-frame primitive.
	 */
	bool SelectedAgentFactsThisFrame(FAgentFacts& Out) const;
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
	 * NO TARGET OR NO TOOL STILL RESETS UNCONDITIONALLY: a frame with neither must leave the
	 * readout EMPTY rather than holding the last gesture's facts on the bar, and that path
	 * never consults FToolReadoutKey - see the null checks at the top of the .cpp. Called from
	 * PlayerTick ahead of the target guard for exactly that reason.
	 *
	 * WITH A TARGET AND A TOOL, REBUILDS ONLY WHEN FToolReadoutKey CHANGED - issue #190. A tool
	 * asked the same question every frame a cursor sits still gave the same answer every time,
	 * paying a TArray<TPair<FString,FString>> and a Printf per fact (BuildReadout's own cost)
	 * for it. InvalidateToolReadoutCache() covers what the key cannot see - a tool's own
	 * internal stage advancing on a click or a drag with the cursor unmoved - by clearing
	 * bHasReadoutKey at every site that mutates gesture or tool state; see its callers.
	 */
	void CollectToolReadout();

	/**
	 * PlaneHit and Tunables exactly as MakeToolContext assembles them, factored out so
	 * PlayerTick can hand the SAME values to FBuildSession::GetFrameContext instead of a second,
	 * hand-copied version of this logic - issue #303. ONE FUNCTION, not two independent
	 * assemblies of "what does the cursor mean right now": CLAUDE.md's "lists that must agree
	 * are ONE list", applied to a computation rather than a table. MakeToolContext still calls
	 * Session.MakeContext directly rather than through the cache - it exists for the handful of
	 * callers (a click, a drag step) that read the mouse position at the moment they fire, and
	 * changing what it means was not this issue's scope.
	 */
	void ComputeCurrentPlaneHitAndTunables(FVector2D& OutPlaneHit, FBuildSessionTunables& OutTunables) const;

	/**
	 * Forces the next CollectToolReadout to rebuild regardless of FToolReadoutKey - issue #190.
	 * Also retires FBuildSession's own frame-context cache (issue #303) - the SAME nine call
	 * sites this method already has cover exactly what that cache cannot see either: a click, a
	 * drag step, a commit, a cancel, an undo or redo, a network cleared out from under the tool.
	 * One list, not two grown side by side to agree by hand - see FBuildSession::
	 * InvalidateFrameContextCache for the risk of a future site missing one of the two clears.
	 *
	 * THE KEY IS A FINGERPRINT OF FToolContext, not of a tool's own member state (PlotPlaceTool's
	 * pinned-corner count and the like): a tool has no generic "stage" a driver could read, and
	 * inventing one to fold into the key would touch every IBuildTool the way CLAUDE.md's "lists
	 * that must agree" warns against. Every call this class makes INTO a tool or the session that
	 * can change what BuildReadout would say - a click, a drag step, a commit, a cancel, an undo
	 * or redo, a tool or mode switch - calls this right after, so the cache never has to guess
	 * whether one of those changed the answer: it just stops trusting last frame's key.
	 */
	void InvalidateToolReadoutCache() { bHasReadoutKey = false; Session.InvalidateFrameContextCache(); }

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

	/**
	 * The lookup and TryRun OnActionKey wraps around a real IsInputKeyDown read - split out so
	 * OnActionKeyForTest can drive it with a SYNTHETIC Ctrl flag instead of a real key-down
	 * state a headless test has no viewport to produce (issue #192).
	 *
	 * FALLS BACK to the plain (bRequiresCtrl=false) action when the exact match misses and Ctrl
	 * WAS held: FindAction used to require bRequiresCtrl to match exactly, so a plain tool key
	 * (e.g. '1') matched nothing while Ctrl was held for an unrelated reason (remove mode's own
	 * modifier) - the key that goes nowhere, CLAUDE.md's own name for this class of bug. Never
	 * tried the other way: an action that REQUIRES Ctrl must never fire without it.
	 */
	void RunActionForKey(FKey Key, bool bCtrl);

	/** Chord bindings carry no key, so Ctrl actions share this and ask which key was just pressed. */
	void OnCtrlActionKey();


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
	 * The five HUD widgets and their configured classes - see UBuildHudLayer's own comment.
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
	 * HasRunway's cache, and how many times it has actually recomputed - see HasRunway's own
	 * comment for why this now exists where one used to argue against it.
	 *
	 * Mutable: HasRunway is const (every other read-side query here is), and a cache behind a
	 * const query is the standard shape for one - the const-ness is a promise about the
	 * OBSERVABLE answer, not about whether answering it may remember work.
	 */
	mutable bool bRunwayCacheValid = false;
	mutable bool bRunwayCache = false;
	mutable int32 RunwayRecomputeCountForTest = 0;

	/** Which facade bRunwayCacheValid is bound to - see BindRunwayCacheInvalidation. Compared
	 *  by pointer so a Target swap (BeginPlay found a different actor than SetTargetForTest
	 *  last pointed at, or vice versa in a test) re-subscribes rather than trusting a stale
	 *  binding to a facade that no longer belongs to Target. */
	TWeakObjectPtr<URoadEditFacade> BoundRunwayCacheFacade;

	/**
	 * Subscribes to Target's facade's OnChanged so a runway PLACED or REMOVED invalidates
	 * bRunwayCacheValid - called from both BeginPlay and SetTargetForTest, the two places
	 * Target is assigned. A no-op (beyond invalidating the cache) when Target is null or its
	 * facade is already the one bound.
	 */
	void BindRunwayCacheInvalidation();

	/** Target's facade's OnChanged handler. GEOMETRY MOVES NOTHING RUNWAY-SHAPED - a dragged
	 *  node cannot create, delete or reclassify a segment - so only Topology can make
	 *  HasRunway's cached answer wrong; see EChangeKind's own comment (Tool/RoadEditTarget.h)
	 *  for the exact split. */
	void OnNetworkChangedInvalidateRunwayCache(EChangeKind Kind);

	/**
	 * This frame's SelectedAgentFacts, computed by SelectedAgentFactsThisFrame at most once
	 * per GFrameCounter tick. TOptional inside TFrameValue: IsSet() alone would only say
	 * "asked this frame", not "an aircraft was actually found" - a stand selected, or an
	 * aircraft that departed between one caller and the next, both have to stay false for
	 * every caller in the frame, not just the first.
	 */
	mutable TFrameValue<TOptional<FAgentFacts>> SelectedAgentFactsCache;

	/**
	 * This frame's readout, refilled by CollectToolReadout.
	 *
	 * NOT A UPROPERTY: FToolReadoutCollector is a plain struct holding FStrings, with no
	 * UObject reference in it, so there is nothing here for the GC to keep alive.
	 */
	FToolReadoutCollector ToolReadoutCollector;

	/**
	 * A cheap fingerprint of everything CollectToolReadout's answer can depend ON - issue #190.
	 *
	 * NOT FToolContext ITSELF: that struct carries a Target pointer and a Selection pointer
	 * that compare equal across frames even when what they point AT has changed, which is
	 * exactly the staleness this key must not have, and it carries Limits and a SnapRadius
	 * that never vary within one session. This names only the values a tool's BuildReadout is
	 * documented to read - Cursor, the snap chain's kind and handle, the guide, and which
	 * handle kind Edit mode exposes - plus which tool is being asked, since a session mode or
	 * tool switch can change the answer with the cursor sitting still.
	 *
	 * WHAT IT CANNOT SEE is a tool's OWN internal stage - FPlotPlaceTool's pinned-corner count
	 * and the like - which is why InvalidateToolReadoutCache() exists: every call this class
	 * makes that could advance one clears the cache directly instead of this struct trying to
	 * fingerprint state no interface exposes.
	 */
	struct FToolReadoutKey
	{
		const IBuildTool* Tool = nullptr;
		FVector2D Cursor = FVector2D::ZeroVector;
		ERoadSnapKind SnapKind = ERoadSnapKind::Free;
		FRoadNodeId SnapNode;
		FRoadSegmentId SnapSegment;
		bool bGuideActive = false;
		FVector2D GuidePoint = FVector2D::ZeroVector;
		EEditHandleKind EditHandles = EEditHandleKind::None;

		bool operator==(const FToolReadoutKey& Other) const
		{
			return Tool == Other.Tool
				&& Cursor == Other.Cursor
				&& SnapKind == Other.SnapKind
				&& SnapNode == Other.SnapNode
				&& SnapSegment == Other.SnapSegment
				&& bGuideActive == Other.bGuideActive
				&& GuidePoint == Other.GuidePoint
				&& EditHandles == Other.EditHandles;
		}
	};

	/** Built from FrameContext and the active tool - see FToolReadoutKey. */
	static FToolReadoutKey MakeReadoutKey(const IBuildTool* Tool, const FToolContext& Context);

	/** Last frame's key, compared in CollectToolReadout. Undefined content when
	 *  bHasReadoutKey is false - see that field. */
	FToolReadoutKey LastReadoutKey;

	/** False before the first successful build and right after InvalidateToolReadoutCache -
	 *  a fresh controller or a just-invalidated one must rebuild rather than compare against
	 *  a LastReadoutKey that happens to read as a match by construction (every FVector2D
	 *  defaults to zero). */
	bool bHasReadoutKey = false;

	/** See GetToolReadoutRevision(). */
	int32 ToolReadoutRevision = 0;

	/** See GetFrameContext(). Built once at the top of PlayerTick; not a UPROPERTY for the
	 *  same reason ToolReadoutCollector above is not one - FToolContext holds no UObject
	 *  reference the GC needs to know about. */
	FToolContext FrameContext;

	// --- Press, drag, release ---------------------------------------------------------
	//
	// The controller decides what the MOUSE did - a click or a drag, and how far a press
	// must travel to count as one. What that MEANS is the tool's business, and a tool
	// never sees a raw key. The recogniser itself is FBuildGesture (Tool/BuildGesture.h),
	// shared with URoadBuildEditorTool - see issue #92; this class keeps only the host I/O
	// of reading the mouse and calling into IBuildTool.

	FBuildGesture Gesture;
};
