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
#include "Model/AgentRescue.h"
#include "Model/FacilityPurchases.h"
#include "RoadBuildController.generated.h"

class FGamePlayerSettingsSink;
struct FBuildAction;
struct FSnapToggleRegistration;

class AAirsideBuildingsActor;
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
 * the six HUD widgets are UBuildHudLayer, a subobject. What remains here is INPUT (binding
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
	 * it WHEN THE CURSOR IS NOT ON ITS BODY - the fallback for an aircraft zoomed out to a few
	 * pixels (see HoverAgentUnderCursor); anywhere on the drawn body picks it regardless. Pixels, not uu: an aircraft on final is clicked in screen space (spec §3.3), and a
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

	// LandAircraftType WAS HERE until 2026-09-27: a Config override naming the one type key 7
	// landed (DefaultGame.ini set it to the 737). Its reason - "not having to wait for an offer
	// to see a particular aeroplane on the runway" - is now the Land panel's whole job, and
	// with key 7 opening the panel nothing read it any more; a setting nothing consumes is the
	// shape CLAUDE.md's "check where a list is CONSUMED" names. Removed, not kept dead. The
	// panel's choice reaches LandAircraftNearViewFocus as its argument instead - still read
	// there and nowhere else, so offers and their arrivals still resolve their own type.

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
	 * Applies one SnapToggleRegistry() entry - a guide relation (ALIGN BY), a reference (SNAP TO),
	 * the Grid button's Off -> 1 m -> 5 m -> 10 m cycle, or the grid's Follow <-> World (H) - to the
	 * airport this controller drives. The bar's snap rows all call this. Logs
	 * "Snap toggle <id> -> <state>" ("snap.grid -> Grid: 5 m"), or that it was ignored with no airport.
	 *
	 * THROUGH THE TARGET, because the settings live on the airport and not on the driver - see
	 * FSnapGuideSettings. A copy here would be a second place for them to drift, which is the
	 * failure FRoadSnapSettings already records.
	 *
	 * ENFORCED BY: AirportMgr.Actions.SnapRowsComeFromTheRegistry (every snap row, run through TryRun,
	 * lands on the airport exactly as the registry's Apply) - for the paragraph below.
	 * ONE DOOR, NOT EIGHT (issue #440): this replaced ToggleGuideRelation/IsGuideRelationOn,
	 * ToggleGuideReference/IsGuideReferenceOn, CycleGridStep/GetGridStepUu and
	 * ToggleGridOrientation/IsGridFollowing - proxies that existed only so game-module rows could
	 * reach settings the editor could not, which is why the editor mode had none of them. The
	 * lit/caption half needs no door at all: a bar row reads Ctx.Target->GuideSources itself.
	 * URoadBuildEdMode::ApplySnapToggle is the editor's twin, from the same table.
	 */
	void ApplySnapToggle(const FSnapToggleRegistration& Toggle);

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

	/** A context carrying only Target - all a variant question reads. See the .cpp. */
	FToolContext MakeVariantContext() const;

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
	void SetTargetForTest(ARoadNetworkActor* InTarget) { Target = InTarget; BindFacadeListeners(); }

	/** The buildings actor BeginPlay would have found beside Target - SetTargetForTest's
	 *  precedent, so a test can watch PlayerTick push ghost visibility at it. */
	void SetBuildingsForTest(AAirsideBuildingsActor* InBuildings);

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

	/** The lit tool's variant rows, for the bar's popout - see FBuildSession::GetActiveVariantAxes. */
	void GetActiveVariantAxes(TArray<FToolVariantAxis>& Out) const;

	/** A bar click on a variant. Logs the pick, taken or refused, under LogRoadBuild. */
	bool SelectActiveVariant(int32 Axis, int32 Option);

	/** Drives RunActionForKey with a SYNTHETIC Ctrl flag, same precedent as PlayerTickForTest:
	 *  a headless test has no viewport to hold a real key down, so OnActionKey's own
	 *  IsInputKeyDown read is bypassed rather than faked. See RunActionForKey's own comment
	 *  for the fallback this exists to prove (issue #192). */
	void OnActionKeyForTest(FKey Key, bool bCtrl) { RunActionForKey(Key, bCtrl); }

	/** A left press at a screen point, without reading a real mouse - OnActionKeyForTest's precedent. */
	void PressPrimaryForTest(FVector2D At) { PressAt(At); }
	bool IsPrimaryPressedForTest() const { return Gesture.IsPressed(); }
	/** EndPlay without tearing the world down - PlayerTickForTest's precedent. */
	void EndPlayForTest() { EndPlay(EEndPlayReason::EndPlayInEditor); }

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

	/** The alerts window (ops alerts spec 2026-09-29) - the bar's Alerts button. */
	void ToggleAlerts();
	bool IsAlertsShowing() const;
	/** How many standing alerts the window holds - the button's count. 0 with no HUD. */
	int32 AlertCount() const;

	/** Open Settings, or cancel it if open. The game.settings action's verb (Escape, the gear). */
	void ToggleSettings();

	/** Whether Settings is open, so the bar's gear can light itself. */
	bool IsSettingsShowing() const;

	/** A modal window is up: every key but Settings' own waits (spec section 2, Modal). */
	bool IsModalOpen() const;

	/** Whether Action's key must wait: a modal is up and it is not Settings' own. */
	bool KeyWaitsForModal(const FBuildAction& Action) const;

	/** Open or close the Land panel. The aircraft.land action's verb (key 7). */
	void ToggleLandPanel();

	/** Whether the Land panel is open, so the bar's Land button can light itself. */
	bool IsLandPanelShowing() const;

	/** Where the build view is looking, on the road plane - forwards to the camera component.
	 *  The Land panel judges its rows against the runway nearest this, as the landing does. */
	FVector2D GetViewFocus() const;

	/** The type the last LandAircraftNearViewFocus call was asked for - the Land panel's
	 *  composition test reads it, since a headless world has no runway to land on. */
	const UAircraftType* GetLastLandRequestForTest() const { return LastLandRequest.Get(); }
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
	 * already owns the board) - this supplies the view focus, the chosen type if any, and the
	 * one path LandNear cannot cover: the editor mode's no-runtime direct dispatch.
	 *
	 * Type is the Land panel's choice (2026-09-27); null lands the content default. Called
	 * from the panel's rows, no longer from key 7, which opens the panel.
	 */
	void LandAircraftNearViewFocus(const UAircraftType* Type = nullptr);

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

	/**
	 * An alert's "Go" (ops alerts spec 2026-09-29 §3): move the camera to the alert's subject and select
	 * it, so the inspector opens on it. An agent is selected as ESelectionKind::Aircraft (the kind the
	 * select tool gives any agent), an entity as Stand, by index. A Point is focus only.
	 *
	 * FALSE, AND NOTHING MOVES, when there is nowhere to go: no focus at all, or a subject gone since the
	 * alert was raised (an agent retired, a stand deleted) - the row stays until the next pass clears it.
	 * ENFORCED BY: AirportMgr.UI.Alerts.GoToSomethingGoneMovesNothing
	 */
	bool SelectAndFocus(const struct FAlertFocus& Focus);
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

	/** A runway is selected (ESelectionKind::Runway) and still describes - the card and the verb. */
	bool SelectedRunwayFacts(FRunwayCardFacts& Out) const;
	bool CanFlipSelectedRunway() const { FRunwayCardFacts Unused; return SelectedRunwayFacts(Unused); }
	/**
	 * Change the selected runway's direction in use to its other end, through the actor's
	 * SetRunwayFacts (so it is one undo step and logs "Runway 09/27 in use: 27 (was 09)").
	 * Flights already planned finish as planned; the next plan reads the new direction
	 * (ruling 2, spec 2026-09-28-runway-in-use).
	 */
	void FlipSelectedRunway();

	/**
	 * Step the selected runway's ERunwayUse on - mixed, arrivals only, departures only, mixed
	 * (RunwayUse::Next) - through the actor's SetRunwayFacts, FlipSelectedRunway's path: one undo
	 * step, logged "Runway at segment N takes: arrivals only (was mixed)". Planned flights keep
	 * their plan; the next plan reads it. Enabled whenever the flip is (CanFlipSelectedRunway).
	 */
	void CycleSelectedRunwayUse();

	/**
	 * THE UNSTICK MENU'S VERBS (spec 2026-09-29-unstick-agent) - FORWARDERS to UOpsRuntime::CanUnstick /
	 * Unstick with the selected agent, so the inspector's lines and the action they run are the one
	 * decision UAgentRescue makes. Refused ("Nothing selected") with no agent selected or no runtime
	 * (the editor mode has none).
	 */
	FUnstickVerdict CanUnstickSelected(EUnstickAction Action) const;
	void UnstickSelected(EUnstickAction Action);

	/**
	 * The selection.unstick row's Execute: ASK the inspector to open its menu. A COUNTER, not a bool the
	 * two sides would have to keep in step: the inspector opens whenever it sees a request it has not,
	 * and the menu closes itself however the player dismisses it - so the bar button and the
	 * inspector's own reach the one popup, and nothing has to be told it closed.
	 */
	void RequestUnstickMenu() { ++UnstickMenuRequests; }
	int32 GetUnstickMenuRequests() const { return UnstickMenuRequests; }

	/**
	 * THE selection->depot WALK, written once (ruling C4 of the facility-upgrades plan): the live fuel
	 * depot a Stand-kind selection names on InTarget's network, else unset. STATIC over (target,
	 * selection) rather than a member reading this controller's own, so a headless test and the ghost
	 * reveal (RevealedDepotFor, which narrows it to PLOTTED depots) ask it without a controller. What
	 * counts as "the selected depot" is decided here; a caller narrows, never re-walks.
	 * ENFORCED BY: AirportMgr.Actions.SelectionNamesItsDepot
	 */
	static FEntityInstanceId DepotForSelection(const ARoadNetworkActor* InTarget, const FSelection& Selection);

	/**
	 * The depot whose ghost slots the selection reveals (facility-upgrades spec R10): DepotForSelection's
	 * answer narrowed to a PLOTTED yard, else unset. PlayerTick hands it to AAirsideBuildingsActor::ShowPlotGhosts.
	 * ENFORCED BY: AirportMgr.Actions.RevealedDepotFollowsTheSelection
	 */
	static FEntityInstanceId RevealedDepotFor(const ARoadNetworkActor* InTarget, const FSelection& Selection);

	/**
	 * THE DEPOT CARD'S VERBS (facility-upgrades spec §4) - FORWARDERS to UOpsRuntime with the selected
	 * depot, the Unstick verbs' shape. The quote is asked fresh on every call, so an enabled check and the
	 * command it guards read the same state. Refused (logged) with no depot selected or no runtime.
	 */
	FEntityInstanceId SelectedFacility() const;
	FFacilityQuote QuoteSelectedFacility() const;
	/** The quote's FIRST module offer - the only one this slice (the shed). A second becomes a menu. */
	bool CanBuySelectedModule() const;
	void BuySelectedModule();
	/** The card's buy menu chooses, then runs selection.buy_vehicle - the row cannot carry the type. */
	void ChooseVehicleToBuy(FName TypeCode) { ChosenVehicleType = TypeCode; }
	bool CanBuyChosenVehicle() const;
	void BuyChosenVehicle();
	/** The fleet row's first click arms, its second runs selection.sell_vehicle. 0 disarms. */
	void ArmSellVehicle(int32 VehicleId) { ArmedSellVehicle = VehicleId; }
	int32 GetArmedSellVehicle() const { return ArmedSellVehicle; }
	bool CanSellArmedVehicle() const;
	void SellArmedVehicle();

	/**
	 * The ops runtime the depot verbs forward to: the one SetOpsRuntimeForTest gave, else the game
	 * instance's (null in the editor mode). A headless world has no game instance, so without the
	 * override the verbs could never be driven end to end by a test.
	 */
	UOpsRuntime* GetOpsRuntime() const;
	/** See GetOpsRuntime. SetTargetForTest's precedent. */
	void SetOpsRuntimeForTest(UOpsRuntime* InRuntime) { OpsRuntimeOverride = InRuntime; }
	/** Writes the session's selection as a Select-tool click would - a headless test has no screen to
	 *  pick from. PlayerTickForTest's precedent. */
	void SelectForTest(const FSelection& InSelection);

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
	/** Puts back what the player's settings changed outside this actor (the engine's UI scale). */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

private:
	/** Left button down: remember where. Decides nothing - that waits for the release. */
	void OnPrimaryPressed();
	/** OnPrimaryPressed's work at a known point - refused under a modal. */
	void PressAt(FVector2D Screen);

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
	 * Also retires FBuildSession's own frame-context cache (issue #303) - the SAME call sites
	 * this method already has (nine on 2026-09-30) cover exactly what that cache cannot see
	 * either: a click, a drag step, a commit, a cancel, and a network replaced out from under the
	 * tool - an undo, a redo, a clear or a load, all through the ONE site OnNetworkReplaced since
	 * #426 (three hand-paired sites before it, and none for a load).
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

	/** The agent whose drawn body the cursor is over (nearest first), else the one whose
	 *  projected position is nearest the cursor within AgentPickPixels, else 0. */
	int32 HoverAgentUnderCursor() const;

	/** View's visible primitives' bounds in its own frame - the body HoverAgentUnderCursor
	 *  picks by. Empty (IsValid false) when nothing is visible. */
	static FBox VisibleLocalBounds(const AActor& View);

	/** The hover HoverAgentUnderCursor last logged, so it logs on change and not per frame. */
	mutable int32 LastLoggedHoverAgent = 0;

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

	/** The player's settings, made at BeginPlay and shared with the Settings window - see
	 *  FGamePlayerSettingsSink. Not a UPROPERTY: a plain C++ object holding only a weak pointer. */
	TSharedPtr<FGamePlayerSettingsSink> SettingsSink;

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
	 * The six HUD widgets and their configured classes - see UBuildHudLayer's own comment.
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

	/** The buildings actor found or spawned beside Target on BeginPlay. Weak: the level owns
	 *  it, and this only pushes ghost visibility at it - see PlayerTick. */
	TWeakObjectPtr<AAirsideBuildingsActor> Buildings;

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

	/** See GetLastLandRequestForTest. Weak: a content asset, owned by nothing here. */
	TWeakObjectPtr<const UAircraftType> LastLandRequest;

	/** Which facade the listeners below are bound to - see BindFacadeListeners. Compared
	 *  by pointer so a Target swap (BeginPlay found a different actor than SetTargetForTest
	 *  last pointed at, or vice versa in a test) re-subscribes rather than trusting a stale
	 *  binding to a facade that no longer belongs to Target. */
	TWeakObjectPtr<URoadEditFacade> BoundRunwayCacheFacade;

	/**
	 * Subscribes to Target's facade: OnChanged, so a runway PLACED or REMOVED invalidates
	 * bRunwayCacheValid, and OnReplaced (#426), so a network replaced by anything - an undo from
	 * the settings dialog, a load from a menu - puts the tool down and retires the caches here.
	 * Called from both BeginPlay and SetTargetForTest, the two places Target is assigned. A
	 * no-op (beyond invalidating the cache) when Target is null or its facade is already the one
	 * bound; the old facade's bindings are removed, so a swapped-away target cannot put this
	 * driver's tool down. Was BindRunwayCacheInvalidation, when the runway cache was all it bound.
	 */
	void BindFacadeListeners();

	/** Target's facade's OnChanged handler. GEOMETRY MOVES NOTHING RUNWAY-SHAPED - a dragged
	 *  node cannot create, delete or reclassify a segment - so only Topology can make
	 *  HasRunway's cached answer wrong; see EChangeKind's own comment (Tool/RoadEditTarget.h)
	 *  for the exact split. A replaced network notifies Topology too (AdoptNetwork), which is
	 *  how a load reaches this cache. */
	void OnNetworkChangedInvalidateRunwayCache(EChangeKind Kind);

	/**
	 * Target's facade's OnReplaced handler - THE ONE PLACE this driver answers a replaced network (issue #426), where
	 * OnUndo, OnRedo and OnClearNetwork each used to deactivate the tool and invalidate the readout by hand and a load
	 * did neither. The session decides what a replacement retires (FBuildSession::OnNetworkReplaced); the readout cache
	 * is this driver's own, retired on Adopted with the frame context.
	 */
	void OnNetworkReplaced(ENetworkReplace Phase);

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
	 * tool switch can change the answer with the cursor sitting still, and the purse's balance
	 * (issue #439), which changes the answer with nothing the player did at all.
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

		/** The grid: pressing Grid changes the readout (a plot's frontage, its letter)
		 *  with the cursor sitting still - review, 2026-09-27. The whole FRAME since
		 *  2026-09-28: pressing H, or the grid turning to a new road, does the same. */
		GridSnap::FGridFrame Grid;

		/**
		 * THE FUNDS a tool's affordability answer was made against - issue #439. Everything above
		 * is something the PLAYER moves; the balance moves on its own (landing fees, upkeep, a
		 * load), and a readout that says "cannot afford" or lights Build is a function of it. Unset
		 * for a target with no purse, which builds for free (IBuildPurse's rule).
		 *
		 * THE BALANCE VALUE, not ULedger::Revision: a revision moves only on the mutations that
		 * remember to bump it (issue #426: a load does not), while equal balances give equal
		 * CanAfford answers for as long as pricing is the identity - see IBuildPurse::Balance for
		 * what ends that. Read through the interface, so this class still names no ledger.
		 * ENFORCED BY: AirportMgr.Actions.ReadoutCacheSeesThePurse
		 */
		TOptional<double> PurseBalance;

		/**
		 * THE TARGET'S EDIT EPOCH (IRoadEditTarget::GetEditEpoch) - issue #439's orchestrator addition.
		 * The other thing that moves under a still cursor: a stand placed or removed by anything but
		 * a call this controller makes (the ops runtime, a scripted edit) changes what a tool would
		 * say about the ground it is over, and the controller invalidates the cache only at the calls
		 * IT makes. One call and an integer compare. Zero for a null target. Like the epoch itself, it
		 * does not see a save-game load - IRoadEditTarget::GetEditEpoch, issue #426.
		 * ENFORCED BY: AirportMgr.Actions.ReadoutCacheSeesThePurse (an edit under an unmoved cursor)
		 */
		uint32 EditEpoch = 0;

		bool operator==(const FToolReadoutKey& Other) const
		{
			return Tool == Other.Tool
				&& Cursor == Other.Cursor
				&& SnapKind == Other.SnapKind
				&& SnapNode == Other.SnapNode
				&& SnapSegment == Other.SnapSegment
				&& bGuideActive == Other.bGuideActive
				&& GuidePoint == Other.GuidePoint
				&& EditHandles == Other.EditHandles
				&& Grid.SameGrid(Other.Grid)
				&& PurseBalance == Other.PurseBalance
				&& EditEpoch == Other.EditEpoch;
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

private:
	/** See RequestUnstickMenu. */
	int32 UnstickMenuRequests = 0;

	/** See ChooseVehicleToBuy / ArmSellVehicle. Session state, never saved. */
	FName ChosenVehicleType;
	int32 ArmedSellVehicle = 0;

	/** See SetOpsRuntimeForTest. Null in play: GetOpsRuntime asks the game instance. */
	UPROPERTY(Transient) TObjectPtr<UOpsRuntime> OpsRuntimeOverride;
};
