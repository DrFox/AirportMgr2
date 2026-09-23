#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/TrafficRules.h"
#include "Build/AnchorLink.h"
#include "Present/RoadSurfacePresenter.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadSnap.h"
#include "Tool/SnapGuideSettings.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class URoadProfile;
class ARoadAgentActor;
class UDynamicMeshComponent;
class UMaterialInterface;
class URoadMaterialSet;
class URoadEditHistory;
class URoadEditFacade;
class UAirsideTraffic;
class UGroundTraffic;
class UTyreSmoke;
class UEntityDefinition;
enum class EAgentPhase : uint8;
enum class EDepartureRefusal : uint8;

// UGroundTraffic FORWARD DECLARED, NOT INCLUDED (issue #175): GetGroundTraffic() below
// returns a bare pointer, which needs no more than that, and this header's own
// FTrafficRules member needs only Model/TrafficRules.h - see that header's own comment.
// Present/AirsideTraffic.h, which every .cpp dereferencing the pointer already includes,
// still pulls in the full Model/GroundTraffic.h.
//
// UEntityDefinition is FORWARD DECLARED THE SAME WAY (issue #191): it is held only behind
// TObjectPtr here, with every accessor returning a bare pointer
// (ResolveStandDefinition/ResolveFuelDepotDefinition/ResolveEntityDefinition/
// GetEntityDefinition). Its complete type still reaches this TU - the class needs it nowhere
// else - via Tool/RoadEditTarget.h below, which already includes Entities/EntityDefinition.h
// as ITS OWN base-interface dependency. (UPlotPresenter was forward declared here for the same
// reason until 2026-09-22, when plots moved to AAirsideBuildingsActor.) Model/RoadHandles.h, Entities/EntityDefinition.h, Tool/RoadHeal.h and
// Profiles/RoadProfile.h used to be listed again here too, redundantly: Tool/RoadEditTarget.h
// (below, mandatory - it is IRoadEditTarget, this actor's base) already includes all four
// directly, so repeating them bought this header nothing and cost every one of its ~65
// includers a second parse of the same four headers. URoadProfile is NOT fully droppable
// the same way: FallbackWidth/FallbackFilletRadius default from
// URoadProfile::StandardTaxiwayWidth/FilletRadius below, a static constexpr member access
// that needs the complete type at THIS file's own class body, not just at the call sites
// Tool/RoadEditTarget.h's includers happen to reach - the forward declaration above stays
// for the pointer members, and the complete type still arrives via Tool/RoadEditTarget.h's
// own Profiles/RoadProfile.h, one copy instead of two.
//
// URoadSurfacePresenter, by contrast, CANNOT lose its own #include above: MakeSurfaceSettings
// and MakeSurfaceSettingsForTest name the nested URoadSurfacePresenter::FSurfaceSettings by
// value, and a nested type has no forward-declaration syntax independent of its outer class -
// the outer class must be complete wherever the nested name is written, inline body or not.
//
// FRoadDeletionPlan (Tool/RoadHeal.h) is returned BY VALUE from PlanNodeDeletion, not held
// by pointer, so issue #191's "if by pointer" condition for it does not apply here - it keeps
// arriving complete, just via Tool/RoadEditTarget.h rather than a second direct include.

/**
 * Owns a road network and renders it as one batched dynamic mesh - the level-resident
 * COMPOSITION ROOT for the three objects issue #32 split out of what used to be a single
 * 1977 + 858 line class: URoadSurfacePresenter (the mesh, the aprons, the ghost preview),
 * URoadEditFacade (every graph mutator, query and undo step), and UAirsideTraffic (agents
 * and dispatch). Every responsibility this class still names below is delegated to exactly
 * one of those three in the .cpp; what remains here is: owning them, owning the components
 * and every level-authored UPROPERTY (because those are what the .umap actually saves),
 * PostRegisterAllComponents and Tick (because only an AActor has either), the content
 * Resolve* functions (because only the actor knows about UAirsideSettings' defaults), and a
 * thin forwarder for every member Blueprint, the game module or a test could already call -
 * see the banner comment above them for why they exist and must not shrink.
 *
 * Multiple inheritance from AActor plus IRoadEditTarget: ordinary UE C++, not a deviation
 * needing justification - IRoadEditTarget is a plain abstract class with no UPROPERTYs and
 * no reflection of its own, so it costs nothing to add to an actor's base list. See that
 * header for why the interface exists at all. Every IRoadEditTarget virtual is implemented
 * here by forwarding to whichever of the three owns the real work - mostly the facade, with
 * UpdateGhost/HideGhost/RebuildMesh going to the presenter and DispatchAgent to traffic; see
 * each forwarder's own one-line comment for which.
 */
UCLASS()
class AIRSIDE_API ARoadNetworkActor : public AActor, public IRoadEditTarget
{
	GENERATED_BODY()

public:
	ARoadNetworkActor();

	/**
	 * The first road network in a world, creating one if there is none.
	 *
	 * Having to drag an actor in before any tool would work was a convenience gap rather
	 * than a design requirement. It stays a PLACEABLE actor, though, and deliberately: an
	 * airport is level content, and being an actor is how the graph gets saved into the
	 * map. One auto-spawned at runtime would be transient, which is the problem rather than
	 * the fix.
	 */
	static ARoadNetworkActor* FindOrCreate(UWorld* World);

	/**
	 * The first road network in a world, or nullptr - the read-only half of FindOrCreate.
	 *
	 * For callers that must not spawn one (a controller's BeginPlay warns and no-ops
	 * instead) or that re-poll every tick (a widget's inbox) - see #104: three call sites
	 * used to run their own TActorIterator scan, one of them every frame with a
	 * const_cast, instead of sharing this one.
	 */
	static ARoadNetworkActor* Find(const UWorld* World);

	/**
	 * Rebuilds the surface from the model, and hides the engine's visualization billboard.
	 *
	 * REBUILDING HERE IS NOT AN OPTIMISATION, IT IS THE INVALIDATION OF A CACHE WE CANNOT
	 * DECLINE. UDynamicMeshComponent holds its mesh as UPROPERTY(Instanced) with no
	 * Transient flag, so the built surface is serialised into the level and comes back on
	 * load. That surface is DERIVED - the graph is the truth - and a persisted derived
	 * value with no invalidation is stale by definition. It stayed stale until an unrelated
	 * rebuild caught up, changing width and material under roads already drawn - see
	 * Airside.Present.MeshIsFreshAfterLoad.
	 *
	 * This hook rather than PostLoad because the mesh component must be REGISTERED before
	 * it will accept one, and rather than BeginPlay because the editor viewport is where
	 * the stale picture was being read. It runs in both worlds for the same reason.
	 *
	 * It does dirty the level on open, which is honest: the saved mesh really did disagree
	 * with the model, and saving now records what is actually on screen.
	 *
	 * The billboard half: USceneComponent::CreateSpriteComponent runs on EVERY OnRegister
	 * and attaches an /Engine/EditorResources/EmptyActor sprite whenever bVisualizeComponent
	 * is set. This actor's mesh is in absolute space, so its transform stays at the world
	 * origin - and a sprite there reads as a node the build tool drew at (0,0), a false
	 * picture. The constructor clears the flag; this catches any component another path
	 * attached.
	 */
	virtual void PostRegisterAllComponents() override;

	/** Solve every node, build the mesh, and push it to the component. Forwards to
	 *  Presenter with a FSurfaceSettings built from this actor's own Resolve* functions and
	 *  level-authored tunables - see URoadSurfacePresenter::Rebuild for the pipeline itself. */
	UFUNCTION(CallInEditor, Category = "Airside")
	virtual void RebuildMesh() override;

	/** Advances Traffic; see UAirsideTraffic::Advance for the handover logic this used to do
	 *  itself. */
	virtual void Tick(float DeltaSeconds) override;

	/** True outside a game world, so dispatched agents move in the editor viewport too. */
	virtual bool ShouldTickIfViewportsOnly() const override;

	/**
	 * Re-points Presenter, Facade and Traffic at THIS actor's own subobjects.
	 *
	 * Duplication - which is how play-in-editor makes its copy of the level, and what
	 * copy/paste does - constructs the copy with bCopyTransientsFromClassDefaults, and
	 * FObjectInitializer::InitProperties then overwrites every Transient property that is
	 * not an instanced reference with the CLASS DEFAULT OBJECT's value. These three are
	 * Transient plain pointers, so a duplicated actor arrived holding the CDO's subobjects:
	 * every PIE click went to the CDO's private network, the CDO rebuilt a mesh nobody could
	 * see, and this actor's own Network stayed null (2026-09-06). The constructor has already
	 * created the right objects by name; this puts the pointers back on them.
	 * Rejected alternative: UPROPERTY(Instanced). It would also survive the copy, but it
	 * changes editor and serialisation semantics for what is runtime-only state, and it
	 * hides the reason in a specifier. See Airside.Present.DuplicatedActorOwnsItsSubobjects.
	 */
	virtual void PostInitProperties() override;

#if WITH_EDITOR
	/**
	 * Marks the resolved-content cache dirty - issue #190. This is the ONE editor signal
	 * that any of MaterialSet, SurfaceMaterial, ApronMaterial, RubberMaterial, GhostMaterial,
	 * Content (via UAirsideSettings' own project settings, not this) or Profile might now
	 * resolve differently, so it is also the only thing allowed to invalidate
	 * ResolveSurfaceMaterial and its siblings' cached answers. See
	 * bResolvedContentDirty's own comment for why every property change - not just the ones
	 * this cache reads - is treated the same: telling them apart buys nothing a rebuild
	 * cannot already tell is unnecessary work, and a missed one would serve stale materials
	 * silently, which is the failure this exists to remove.
	 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/**
	 * Hand the presenter this actor's six surface components, indexed by ESurfaceLayer.
	 *
	 * Called from the constructor and AGAIN from PostRegisterAllComponents - see the comment
	 * there for why a saved level can otherwise leave a newly added layer null forever, and
	 * why PostInitProperties is too early to repair it.
	 */
	void InitialisePresenterLayers();

	// --- Agents ----------------------------------------------------------------------
	//
	// Runtime only, and owned by Traffic rather than by this actor or by URoadNetwork - see
	// UAirsideTraffic's class comment for why an agent belongs to neither.

	/** Lands an aircraft on the runway nearest a point and taxis it to a stand. Forwards to
	 *  Traffic - see UAirsideTraffic::DispatchArrival for the arm/spawn/log this actor used
	 *  to do itself, and Model/ArrivalPlanner for which runway, exit and stand are chosen. */
	bool DispatchArrival(const FVector2D& Near, const FAirframe& Airframe);

	/** Sends one agent along a plan, spawning the cube that shows it. Forwards to Traffic.
	 *  The `using` keeps IRoadEditTarget's two-argument (Aircraft) overload visible on this
	 *  type: overriding one signature would otherwise HIDE the other for every caller
	 *  holding an ARoadNetworkActor*, which is most of the tests. */
	using IRoadEditTarget::DispatchAgent;
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe,
		ETraversalClass Class) override;
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FVehicle& Vehicle,
		ETraversalClass Class) override;

	/** Removes every agent and its cube. Forwards to Traffic. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Airside")
	void ClearAgents();

	/** How many agents are currently under way or parked at their destination. Forwards to
	 *  Traffic. Also what Airside.Present.ArrivalDispatch reads - AgentCountForTest was an
	 *  exact duplicate of this and was deleted by issue #80. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 GetAgentCount() const;

	/** The most recently dispatched agent's actor, or null. Forwards to Traffic. */
	ARoadAgentActor* GetNewestAgent() const;

	/** Sends a parked agent to the runway and arms its take-off. Forwards to Traffic. */
	EDepartureRefusal DepartAgent(int32 AgentId);

	/** The actor showing an agent, or null. Forwards to Traffic. */
	ARoadAgentActor* GetAgentView(int32 AgentId) const;

	/**
	 * The traffic mediator, for AirportOps to bind its delegates. READ ACCESS TO A SUBOBJECT,
	 * not a forwarder per delegate: the actor is a composition root that grows by forwarding
	 * (CLAUDE.md), and a forwarder per event would re-grow it one line per event for ever.
	 */
	UAirsideTraffic* GetTraffic() const { return Traffic; }

	/**
	 * Non-const overload of the IRoadEditTarget accessor above, for a caller that needs to
	 * MUTATE traffic (UFlightBoard::AcceptImmediate, the offer inbox's Refresh) rather than
	 * read it. Replaces the `GetTraffic() != nullptr ? GetTraffic()->GetModel() : nullptr`
	 * ternary those call sites used to hand-roll (#103).
	 */
	UGroundTraffic* GetGroundTraffic();

	/**
	 * The surface presenter, for a caller that wants it directly rather than through a
	 * forwarder on this actor. Added alongside GetTraffic() by issue #80 for the same reason:
	 * read access to a subobject, not a forwarder per method - and used exactly that way by
	 * MeshFreshnessTest.cpp and RunwaySurfaceTest.cpp (Actor->GetPresenter()->
	 * SurfaceTriangleCountForTest() and friends) once code review pointed out that keeping the
	 * three-line forwarders AND this accessor was the growth this whole issue was about.
	 */
	URoadSurfacePresenter* GetPresenter() const { return Presenter; }

	/**
	 * Fired after every TOPOLOGY rebuild, once the surface is built - where the plot boxes
	 * used to be drawn by a direct call on this actor.
	 *
	 * A DELEGATE, NOT A POINTER TO THE BUILDINGS ACTOR, so this actor does not know buildings
	 * exist: a depot's sheds are objects standing on the airport, not road network, and this
	 * class reached 2313 lines by owning everything that stood on it. Geometry (drag-frame)
	 * and Markings rebuilds do not fire it, for the reason RebuildMeshForChange gives - nothing
	 * a listener derives from has moved. Native, not dynamic: it carries a const reference,
	 * and nothing in Blueprint listens.
	 *
	 * ENFORCED BY: Airside.Present.BuildingsActorDrawsThroughTheDelegate.
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnTopologyRebuilt, const URoadNetwork&);
	FOnTopologyRebuilt OnTopologyRebuilt;

	/**
	 * Every graph mutator, query and undo step - see URoadEditFacade.
	 *
	 * READ ACCESS TO THE SUBOBJECT, not a forwarder per method, for exactly the reason
	 * GetPresenter above gives. The ops runtime reaches through it at attach to hand the facade
	 * its build purse, and the purse tests reach through it to substitute a recorder.
	 */
	URoadEditFacade* GetEditFacade() const { return Facade; }

	/**
	 * Multiplier applied to every Tick's DeltaSeconds before it reaches Traffic. Set each
	 * frame by AirportOps from the sim clock's SPEED (x0..x8), never from its day
	 * compression - see USimClock's class comment for why the two are different numbers.
	 * Transient and runtime-only: it is a fact about the current session's speed setting,
	 * not about the level, so it must not be saved into the map or a game save.
	 */
	void SetSimTimeScale(double Scale) { SimTimeScale = FMath::Max(0.0, Scale); }

	/**
	 * Sets the delta evening and resets what it has accumulated. 1.0 hands the raw frame
	 * delta through, which is the only way a test can measure what the evening removes.
	 *
	 * RETARGETED to Traffic by issue #80: the running state (SmoothedSeconds/OwedSeconds)
	 * moved into FFrameDeltaSmoother, owned by UAirsideTraffic, so it can be pinned by a
	 * world-free test - see FFrameDeltaSmoother's own header. DeltaSmoothingRate itself stays
	 * here, level-authored, and travels into Traffic BY VALUE every Tick, the same pattern
	 * TrafficRules already uses.
	 */
	void SetDeltaSmoothingForTest(double Rate);
	double GetSimTimeScale() const { return SimTimeScale; }

	/** Route between two guideline nodes over the network this actor owns. Forwards to the
	 *  facade, so a tool, a Blueprint and the HUD all ask the same question of the same
	 *  graph rather than three of them reaching past it. */
	virtual FRoutePlan FindRoute(
		FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan,
		ERouteErrand Errand = ERouteErrand::PlayerIssued) const override;

	// =====================================================================================
	// THIN FORWARDERS. Every member below existed on this actor before issue #32 and is kept
	// here, unchanged in name and signature, purely so Blueprint graphs, RoadBuildController,
	// RoadBuildHUD, RoadBuildEditorTool and every existing automation test compile and behave
	// exactly as they did - none of them may be asked to call Facade or Presenter directly. A
	// later cleanup may repoint those drivers at the facade once this settles; this task is a
	// pure refactor and must not be the one that does it. Each forwarder's own body is one
	// line; the real work, and the WHY comments that used to sit here, moved with the code -
	// see URoadEditFacade.cpp.
	// =====================================================================================

	/**
	 * The build purse and the quote a tool prices its ghost with - forwarded to the facade,
	 * like every other IRoadEditTarget member.
	 *
	 * FORWARDED AND NOT INHERITED FROM THE DEFAULT. FToolContext::Target is THIS ACTOR, so a
	 * tool asking the interface gets the actor's answer; leaving these to IRoadEditTarget's
	 * null default meant the ghost silently priced nothing, with the facade's own purse sitting
	 * right there behind it.
	 */
	virtual IBuildPurse* GetPurse() const override;
	virtual FBuildQuote QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
		int32 WidthIndex) const override;

	/** Add a node at a world-space XY position. Returns its index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 PlaceNode(FVector2D Where) override;

	/** Join two placed nodes with a straight segment. Returns false, and logs, if it refused. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex) override;
	using IRoadEditTarget::ConnectNodes;

	/** Link two GUIDELINE nodes by hand. Returns the new edge's index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) override;

	/**
	 * Lays a runway from From to To in one edit, with its own profile and its facts.
	 *
	 * The three-argument form is the UFUNCTION (UHT allows no overloads) and lays tarmac /
	 * visual - the facts' defaults, which is what every caller before the facts existed
	 * meant; the four-argument form is the interface's, which the tool calls.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile);
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts) override;

	/** Reclassify a runway's whole strip - surface and approach - as one undoable edit. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts) override;

	/**
	 * The shortest thing that may be called a runway, in uu. 500 m.
	 *
	 * Not an aviation rule - real minima depend on the aircraft - but a floor that separates
	 * a runway from a slip of the mouse. A Meridian needs about 700 m at sea level. Stays on
	 * the actor: it is level-authored, and the facade only reads it (see
	 * URoadEditFacade::GetMinimumRunwayLength).
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1.0"))
	double MinimumRunwayLength = 50000.0;

	/** IRoadEditTarget accessor for MinimumRunwayLength - see the property's own comment. */
	virtual double GetMinimumRunwayLength() const override { return MinimumRunwayLength; }

	/**
	 * Content only, like ResolveRunwayMaterial below - there is no per-actor override, because
	 * the set of legal widths is a project-wide fact, not a level one.
	 */
	virtual int32 GetRunwayProfileCount() const override;

	/**
	 * Clamped rather than checked, like the tool's own old ProfileForWidth used to be: the
	 * list is content, so it can be shorter than an index left over from a longer one, and
	 * wrapping would silently resolve a different width from the one shown. Null when the
	 * content set has no runway profiles at all.
	 */
	virtual URoadProfile* ResolveRunwayProfile(int32 Index) const override;

	/** The standard taxiway widths, from the content set - see IRoadEditTarget. */
	virtual int32 GetTaxiwayProfileCount() const override;
	virtual URoadProfile* ResolveTaxiwayProfile(int32 Index) const override;

	/** See IRoadEditTarget::ResolveProfileFor - the one place this rule lives. */
	virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) override;

	/**
	 * The depot kit table, from the content set - see IRoadEditTarget::ResolveDepotKits.
	 *
	 * THE SAME METHOD UPlotPresenter's KITS COME FROM (AAirsideBuildingsActor passes them in),
	 * through this actor rather than through
	 * DepotKitSpecs(UAirsideSettings::GetContent()) a second time (issue #181) - a presenter
	 * and a tool resolving the table independently is the split the reservation design (#180)
	 * exists to prevent, in a new place.
	 */
	virtual TArray<PlotYard::FKitSpec> ResolveDepotKits() const override;

	/** Remove a HAND-AUTHORED guideline edge. Refuses a derived one. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DisconnectGuideline(int32 EdgeIndex) override;

	/** Place (bSet) or clear an intermediate holding position at a guideline node. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet) override;

	/**
	 * DEPRECATED NAME, kept so a Blueprint that bound "SetHoldShort" still compiles - the
	 * refactor contract: every UFUNCTION stays reachable at its old name as a forwarder.
	 * "Hold short" is an ATC instruction, not a place; the place is a holding position
	 * (spec 2026-09-07). New callers use SetIntermediateHoldingPosition.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside", meta = (DeprecatedFunction, DeprecationMessage = "Use SetIntermediateHoldingPosition"))
	bool SetHoldShort(int32 NodeIndex, int32 SegmentIndex) { return SetIntermediateHoldingPosition(NodeIndex, SegmentIndex != INDEX_NONE); }

	/** Index of the nearest live node within Radius of Where, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 FindNodeNear(FVector2D Where, double Radius) const;

	/** Replace a live segment with two, meeting at a new node placed at At. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) override;

	/** Remove a node, rejoining the roads it would otherwise strand. See RoadHeal.h. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DeleteNode(int32 NodeIndex) override;

	/** Remove one segment. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DeleteSegment(int32 SegmentIndex) override;

	/** Slot indices of the segments that deleting NodeIndex would take with it. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	TArray<int32> SegmentsIncidentTo(int32 NodeIndex) const;

	/** Move a node, dragging its roads with it. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) override;

	/** Folds the node in hand into the node it was dropped on. Forwards to the facade. */
	UFUNCTION(BlueprintCallable, Category = "Airside|Road")
	virtual bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex) override;

	/** Moves one corner of an apron outline. Forwards to the facade. */
	UFUNCTION(BlueprintCallable, Category = "Airside|Road")
	virtual bool MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To) override;

	/** Open an edit that spans frames, for a drag. */
	virtual void BeginInteractiveEdit(const FString& Label) override;

	/** Close it. bKeep false abandons the snapshot, leaving no undo step. */
	virtual void EndInteractiveEdit(bool bKeep) override;

	/** What deleting NodeIndex would do, without doing any of it. For the overlay. */
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const override;

	/**
	 * Shortest segment and tightest corner a click may build, and the limits the deletion
	 * plan judges its rejoins against.
	 *
	 * PER-AIRPORT now, not per-driver - issue #93. Both `ARoadBuildController` and
	 * `URoadBuildEditorTool` used to hold their own copy (the controller as UPROPERTYs
	 * `MinSegmentLength`/`MinTurnDegrees`; the editor tool never set either, so it accepted
	 * corners PIE would refuse). `MakeTunables` is the one place both read this now.
	 * NewRoadHalfWidth is the exception: not authored, refreshed IN PLACE by every
	 * `MakeTunables` call from whichever profile is live - see that field's own comment -
	 * so `URoadEditFacade::PlanNodeDeletion`, which reads this member directly rather than
	 * through a Tunables bundle, still judges a rejoin against the current road width.
	 *
	 * Still a plain UPROPERTY rather than moved to the facade: RoadBuildController used to
	 * write the whole struct here directly every frame (Target->PlacementLimits = ...);
	 * MakeTunables replaces that write, but the field stays on the actor for the same
	 * reason it always did - the facade reads it back through its owning actor, see
	 * URoadEditFacade::MoveNode.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Placement")
	FRoadPlacementLimits PlacementLimits;

	/**
	 * Radii and toggles the snap chain judges a click against - see FRoadSnapSettings.
	 *
	 * PER-AIRPORT, not per-driver - issue #93; see FRoadSnapSettings' own comment for the
	 * divergence this replaced. ToolPickRadius is deliberately NOT here: it answers "what is
	 * the cursor pointing at", a screen-scale question each driver judges from its own view,
	 * not an airport fact - see ARoadBuildController::ToolPickRadius.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Placement")
	FRoadSnapSettings Snap;

	/**
	 * Which guide sources the player has switched on.
	 *
	 * PER AIRPORT, beside Snap and for the reason that property records: the editor mode and
	 * PIE must agree about what is live, and a per-driver copy is how the two came to disagree
	 * about snap radii before issue #93.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Placement")
	FSnapGuideSettings GuideSources;

	/**
	 * Snap and placement tunables, for a driver-supplied view scale, as one bundle - see
	 * FBuildSessionTunables. THE ONE PLACE both drivers assemble this now: before issue #93,
	 * ARoadBuildController filled Tunables.Snap/Limits from its own seven UPROPERTYs every
	 * tick, and URoadBuildEditorTool built a DIFFERENT set from a view-derived radius, leaving
	 * Limits at struct defaults entirely - the same click was judged by different rules
	 * depending on which driver was open.
	 *
	 * ViewWorldWidth > 0 asks for an adaptive ToolPickRadius sized off it (what the editor
	 * tool needs, having no view-distance UPROPERTY of its own to read); 0 leaves
	 * ToolPickRadius at its class default for a caller - the runtime driver - that overwrites
	 * it right after with its own ToolPickRadius view fact. Not const: resolving the
	 * corner-fit half-width goes through ResolveProfile, which is deliberately non-const -
	 * see that method's own comment.
	 */
	FBuildSessionTunables MakeTunables(double ViewWorldWidth);

	/** Both endpoints of a live segment, on the road plane. False if it is not live. */
	bool GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const;

	// --- Aprons -----------------------------------------------------------------------

	/** Add a polygon of pavement. Returns its slot index, or INDEX_NONE if refused. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 AddApron(const TArray<FVector2D>& Outline) override;

	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DeleteApron(int32 ApronIndex) override;

	/** The topmost apron containing a point, or INDEX_NONE. For picking. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 FindApronAt(FVector2D Where) const override;

	// --- Stands -----------------------------------------------------------------------

	/** Place a stand, facing Heading in radians. Returns its slot index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) override;
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>& Outline,
		FVector2D FrontageA, FVector2D FrontageB,
		const TArray<EDepotModule>& Modules, EPlaceableEntity Kind) override;
	using IRoadEditTarget::PlaceStand;

	/** Remove a placed entity, and the anchor nodes it owns. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DeleteEntity(int32 EntityIndex) override;

	/** Nearest placed entity within Radius of a point, or INDEX_NONE. For picking. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const override;

	/**
	 * The stand layout new stands are placed from. Defaults to DA_Stand_CodeC.
	 *
	 * A Flyweight: every stand shares one definition and carries only its own pose, which
	 * is the whole reason anchors live on the definition rather than on the instance.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Stands")
	TObjectPtr<UEntityDefinition> StandDefinition;

	/**
	 * What the fuel depot tool places. Unset falls back to the content set's Placeables map
	 * (UAirsideSettings::ResolvePlaceable) - see UAirsideContent::Placeables, which issue #192
	 * item 1 gave the same map treatment this comment already argues against giving THIS pair.
	 *
	 * BESIDE StandDefinition rather than in a map keyed by EPlaceableEntity: there are two
	 * kinds, and two asset pickers in the Details panel are easier to author than a map, for
	 * no loss until a third arrives. That argument was about the PER-ACTOR override, which
	 * still has only two authors ever wanting to set by hand; the CONTENT set's default is a
	 * different question, answered once for every actor, which is exactly where a third kind
	 * would otherwise need a third named property and a ternary to match it.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Stands")
	TObjectPtr<UEntityDefinition> FuelDepotDefinition;

	/**
	 * IRoadEditTarget accessor for StandDefinition - RESOLVED, via ResolveStandDefinition(),
	 * the same as PlaceStand places from: preview and placement must resolve the same
	 * object, or a stand's ghost and the stand PlaceStand actually drops can disagree.
	 */
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity Kind) const override
	{
		return ResolveEntityDefinition(Kind);
	}
	using IRoadEditTarget::GetStandDefinition;

	/** Discard the whole graph and the mesh built from it. Undoable. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	void ClearNetwork();

	// --- Undo -------------------------------------------------------------------------

	/** Take back the last edit. False when there is nothing to take back. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool Undo();

	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool Redo();

	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool CanUndo() const;

	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool CanRedo() const;

	/** Name of the edit the next Undo would take back, for the overlay. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	FString PeekUndoLabel() const;

	/** How many edits can be taken back before the oldest is forgotten. */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1"))
	int32 MaxUndoDepth = 50;

	// --- Ghost preview --------------------------------------------------------------

	/**
	 * Show the segment a click would build, as real solved pavement. Forwards to Presenter
	 * with a FSurfaceSettings built the same way RebuildMesh's is.
	 *
	 * Parameter named SnapResult, not Snap: this class now also has a Snap member
	 * (ARoadNetworkActor::Snap, the per-airport FRoadSnapSettings - issue #93), and a
	 * same-named parameter would shadow it.
	 */
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& SnapResult, bool bValid,
		ERoadKind Kind, int32 WidthIndex) override;
	using IRoadEditTarget::UpdateGhost;

	/**
	 * The ghost's triangles, without touching a component, a material or a renderer.
	 *
	 * Public and separated from UpdateGhost so the one property this whole mechanism
	 * rests on can be asserted in a test with no World: building a preview must leave the
	 * REAL network bitwise unchanged. Forwards to Presenter.
	 */
	bool BuildGhostBuffers(int32 FromNodeIndex, const FRoadSnapResult& SnapResult, FRoadMeshBuffers& OutBuffers);

	/** Hide the preview and forget what it was showing. Forwards to Presenter. */
	virtual void HideGhost() override;

	/** A live node's handle from its slot index, or false if it is not live. Forwards to
	 *  the facade. */
	virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const override;

	/**
	 * Cross-section for segments created through this facade. When unset, a symmetric
	 * one from FallbackWidth and FallbackFilletRadius is made on demand - the solver
	 * cannot produce a boundary without half-widths, so there is no useful null case.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	TObjectPtr<URoadProfile> Profile;

	/**
	 * Cross-section for SERVICE ROADS laid through this facade - see ERoadKind. Unset falls
	 * back to the content set's ServiceRoadProfile.
	 *
	 * A SECOND PROPERTY rather than a map keyed by kind: there are two kinds, and two asset
	 * pickers in the Details panel are easier to author than a map, for no loss until a
	 * third kind exists.
	 *
	 * NO FallbackWidth TWIN, unlike Profile. That property's on-demand RuntimeProfile exists
	 * so the FIRST click of a session lays something; a road that fell back to a transient
	 * profile would come back from a save as a TAXIWAY (see UAirsideContent::ServiceRoadProfile
	 * for why that is worse than nothing), so the road tool refuses instead and names the
	 * asset that is missing.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	TObjectPtr<URoadProfile> ServiceRoadProfile;

	/**
	 * Material for the road surface. Defaults to M_RoadSurface, which reads UV0 for
	 * asphalt and UV1 for markings. Left null, the surface falls back to the engine
	 * default - which is WorldGridMaterial, the same world-aligned checker the template
	 * floor uses, so the road becomes very hard to tell apart from the ground.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/**
	 * DIAGNOSTIC ONLY. Hold vertex colours at a constant - and, as a side effect nobody
	 * would guess, stop the real material rendering at all.
	 *
	 * Any ColorOverrideMode other than None makes FBaseDynamicMeshSceneProxy set
	 * ForceOverrideMaterial to the engine's vertex-colour debug material, which then
	 * replaces SurfaceMaterial for every buffer set. So this does not tint the surface;
	 * it substitutes a different material entirely and shows a flat constant colour with
	 * no texture, whatever SurfaceMaterial says.
	 *
	 * Default false, because true means "do not render the material you asked for". It
	 * stays available because it is a genuine way to prove geometry reaches the screen
	 * when the material is suspect - just never mistake the result for the material
	 * working. See FDynamicMeshSink::Accept for where this is actually applied.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	bool bUseConstantVertexColour = false;

	/**
	 * Deliberately far narrower than a real taxiway's 2300 uu. A corner needs roughly
	 * five times the road's width in segment length before its fillet has room to be a
	 * curve rather than a clamped-away stub.
	 *
	 * 23 m and a 15 m fillet - a real taxiway, matching the debug gallery - since a real
	 * airframe arrived. At the old 2 m the Piper's 13.1 m wingspan was six times the width
	 * of the road it was taxiing down, which reads as a broken model rather than as a
	 * placeholder road. Roads must now be drawn a few thousand uu a click to avoid the
	 * solver clamping their fillets away, which is what an airport is anyway.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1.0"))
	double FallbackWidth = URoadProfile::StandardTaxiwayWidth;

	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.0"))
	double FallbackFilletRadius = URoadProfile::StandardTaxiwayFilletRadius;

	UPROPERTY(VisibleAnywhere, Category = "Airside")
	TObjectPtr<UDynamicMeshComponent> MeshComponent;

	/**
	 * Third component, carrying the aprons.
	 *
	 * Its own component because an apron shares nothing with a road: no cross-section, no
	 * junction solve, and no vertices that may weld to a road's. Separate also means a
	 * change to one surface cannot force the other to rebuild.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Apron")
	TObjectPtr<UDynamicMeshComponent> ApronComponent;

	/**
	 * The holding-position paint: a fourth surface, half a unit above the road, drawn with
	 * the road's own material - see FHoldingPositionMarkingBuilder. Its own component for
	 * the same reason the apron has one: a rebuild of the roads must not be a rebuild of
	 * everything that happens to be painted on them.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Markings")
	TObjectPtr<UDynamicMeshComponent> MarkingComponent;

	/**
	 * The runway paint: a fifth surface, at the holding positions' height, drawn WHITE
	 * through a dynamic instance of the road material - see
	 * URoadSurfacePresenter::RebuildRunwayMarkings for why the colour needs a component.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Markings")
	TObjectPtr<UDynamicMeshComponent> RunwayMarkingComponent;

	/**
	 * The tyre rubber: a sixth surface, a quarter unit up - under the paint, over the
	 * pavement. Its own component because it is TRANSLUCENT and the other two are opaque,
	 * and blend mode is a property of the material, not of the draw.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Markings")
	TObjectPtr<UDynamicMeshComponent> RunwayRubberComponent;

	/**
	 * Name -> material for the road surface's profile bands. Null renders exactly as
	 * before: one material, every triangle id 0.
	 *
	 * A DataAsset rather than a table edited here, because this actor lives in a level
	 * that is deliberately never saved - see URoadMaterialSet.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TObjectPtr<URoadMaterialSet> MaterialSet;

	/**
	 * Concrete for the aprons. Defaults to M_ApronConcrete.
	 *
	 * A material of its own rather than the road's, and not only for realism: while an
	 * apron borrowed M_RoadSurface it was very hard to tell from the taxiway lying on it
	 * and from the ground under it, which is indistinguishable from it not rendering.
	 *
	 * Left null it falls back to SurfaceMaterial, and if that is null too the sink gives
	 * the component the engine default - which is WorldGridMaterial, the same checker the
	 * template floor wears. That degrades quietly, and quiet is the problem.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Apron")
	TObjectPtr<UMaterialInterface> ApronMaterial;

	/**
	 * The tyre rubber's material. Left null it falls back to UAirsideContent::RubberMaterial,
	 * and if THAT is null the rubber is simply not drawn - which, unlike the apron's
	 * fallback, is a fine outcome: a runway with no rubber looks newly laid, where an apron
	 * with no material looks like a bug.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Markings")
	TObjectPtr<UMaterialInterface> RubberMaterial;

	/** The touchdown puff's material. Null falls back to UAirsideContent::TyreSmokeMaterial,
	 *  and null there means aircraft land without smoking. */
	UPROPERTY(EditAnywhere, Category = "Airside|Markings")
	TObjectPtr<UMaterialInterface> TyreSmokeMaterial;

	/** The touchdown puffs - see UTyreSmoke. A subobject like Traffic, because it
	 *  owns components and a lifetime, and the actor only forwards to it. */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Markings")
	TObjectPtr<UTyreSmoke> Smoke;

	/**
	 * DIAGNOSTIC ONLY. Hold the aprons' vertex colours at a constant - and, as a side
	 * effect nobody would guess, stop ApronMaterial rendering at all.
	 *
	 * The same trap as bUseConstantVertexColour: any ColorOverrideMode other than None
	 * makes the scene proxy substitute the engine's vertex-colour debug material, so this
	 * does not tint the concrete, it replaces it. Which is exactly what makes it useful -
	 * it is the fastest way to answer "is the apron on screen at all", because a flat
	 * unmissable colour cannot be confused with the ground or with the road.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Apron")
	bool bUseConstantApronColour = false;

	/**
	 * Draw every apron triangle as debug lines.
	 *
	 * The same ground truth bDebugDrawMesh gives the roads: the same buffers reaching the
	 * screen by a completely separate route.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Apron")
	bool bDebugDrawAprons = false;

	/**
	 * MOST the aprons sit below the road surface, in uu. Not a fixed drop - see
	 * GetApronSurfaceZ.
	 *
	 * Below, not above: a taxiway crossing an apron should win the depth test, which is
	 * also how it reads in life - the taxiway is painted onto the apron. Coplanar would
	 * z-fight, and the two surfaces genuinely do overlap wherever a road runs onto a stand.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Apron", meta = (ClampMin = "0.0"))
	double ApronZOffset = 4.0;

	/**
	 * Height the apron surface is actually built at. Forwards to Presenter, which owns the
	 * ApronZOffset-as-maximum failure story in full - see
	 * URoadSurfacePresenter::GetApronSurfaceZ.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside|Apron")
	double GetApronSurfaceZ() const;

	/** Second component, carrying only the preview. Separate so showing and hiding the
	 *  ghost never touches the real road's mesh. */
	UPROPERTY(VisibleAnywhere, Category = "Airside|Ghost")
	TObjectPtr<UDynamicMeshComponent> GhostComponent;

	/** Translucent unlit preview material. Defaults to M_RoadGhost. */
	UPROPERTY(EditAnywhere, Category = "Airside|Ghost")
	TObjectPtr<UMaterialInterface> GhostMaterial;

	/**
	 * How far above the road surface the ghost sits, in uu.
	 *
	 * Enough to clear the pavement's depth, little enough that it still reads as lying on
	 * it. At zero the two surfaces z-fight; the preview then flickers rather than hovers.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Ghost", meta = (ClampMin = "0.0"))
	double GhostZOffset = 2.0;

	/** The graph this actor owns and renders. Readable from Blueprint; mutate it only
	 *  through the facade above, so every change stays undoable. Stays on the actor rather
	 *  than moving to URoadEditFacade because this is what the level actually saves - see
	 *  that class's header comment. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside") TObjectPtr<URoadNetwork> Network;

	/** IRoadEditTarget accessor for Network - see the property's own comment. */
	virtual const URoadNetwork* GetNetwork() const override { return Network; }

	/** IRoadEditTarget accessor for the agents: Traffic's model. Forwards to Traffic. */
	virtual const UGroundTraffic* GetGroundTraffic() const override;

	/** Snapshots of the graph before each edit. See URoadEditHistory for why Memento
	 *  rather than the Command layer design spec 7.3 specifies. Stays on the actor for the
	 *  same saved-with-the-level reason as Network. */
	UPROPERTY() TObjectPtr<URoadEditHistory> History;

	/**
	 * Footprints, gaps, stall and retry clocks - every number the traffic arbiter works in.
	 * Spec 2026-09-06 §2.3; handed to UAirsideTraffic::Advance each tick.
	 *
	 * HERE AND NOT ON UGroundTraffic, which is where they used to be EditAnywhere: that
	 * object is Transient and re-created per session, so a figure tuned in the Details panel
	 * was never saved and never survived a PIE duplication. This actor is what the .umap
	 * actually saves, which makes it the only place a level-authored figure can live - the
	 * same reasoning as Network, History and ShutdownPauseSeconds.
	 *
	 * NOT UAirsideSettings either. That class resolves CONTENT defaults - which mesh, which
	 * material, which airframe - in exactly one function each. These are per-airport gameplay
	 * tuning a designer sets on the level, not a default asset to fall back on.
	 *
	 * PUBLIC, unlike ShutdownPauseSeconds: the level authors it and the seam test reads it
	 * back off the model, so it is not a figure this actor keeps to itself.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Traffic") FTrafficRules TrafficRules;

	/**
	 * How far a SERVICE connection may reach, in any direction, uu. 65 m by default, which is
	 * 50 m from where a Code C stand's edge is drawn plus the 15 m its lane sits inboard of
	 * that line - the derivation is on FAnchorLink::DefaultServiceLinkRadius.
	 *
	 * HERE AND NOT A CONSTANT, for the same reason TrafficRules is here: it is per-airport
	 * gameplay tuning a designer sets on the level, not a content default to fall back on
	 * (which is UAirsideSettings' business) and not a fact about a painted line (which is
	 * FAnchorLink::DefaultMaxLeadIn, and stays a constant). An airport laid out with wide
	 * service margins raises it; one that wants a stand to connect only to the road right
	 * beside it lowers it.
	 *
	 * SHORT BY DEFAULT ON PURPOSE - see FAnchorLink::DefaultServiceLinkRadius for why a long
	 * reach reintroduces exactly the failure the ray rule was protecting against.
	 *
	 * PUBLIC, like TrafficRules: the level authors it and the seam test reads it back off the
	 * settings the presenter is actually handed.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Traffic")
	double ServiceLinkRadius = FAnchorLink::DefaultServiceLinkRadius;

	/**
	 * MakeSurfaceSettings, for the seam test.
	 *
	 * The settings struct is the ONLY route from a level-authored figure to the build, so the
	 * test reads THAT rather than re-deriving the number - which is the whole "check where a
	 * list is CONSUMED" rule applied to a single knob.
	 */
	URoadSurfacePresenter::FSurfaceSettings MakeSurfaceSettingsForTest() { return MakeSurfaceSettings(); }

private:
	/** Profile made on demand when none is authored. Transient so it is never saved. */
	UPROPERTY(Transient) TObjectPtr<URoadProfile> RuntimeProfile;

	/**
	 * MakeSurfaceSettings' own cache of the resolvers that actually pay for a
	 * LoadSynchronous - SurfaceMaterial, ApronMaterial, RubberMaterial, GhostMaterial and
	 * the three runway surfaces (issue #190). ResolveMaterialSet and ResolveProfile are not
	 * cached here: neither ever calls LoadSynchronous (see each one's own body), so there is
	 * nothing this cache would save them.
	 *
	 * STARTS DIRTY, not with a null-means-unresolved convention: a resolved-and-null answer
	 * (no content set configured) is cached exactly like any other, which is what "a
	 * null-material fallback must still resolve on the next rebuild" is about - a null
	 * CACHED VALUE must still read as "already answered", never as "go and ask again". Only
	 * PostEditChangeProperty is allowed to set this back to true; MakeSurfaceSettings never
	 * clears its own dirty flag from the outside.
	 *
	 * UPROPERTY(Transient), NOT a bare bool, and that is load-bearing rather than
	 * decorative - see PostInitProperties' own comment on why a Transient plain pointer
	 * comes back as the CLASS DEFAULT OBJECT's value after a PIE duplication. The cache
	 * pointers below are Transient for the same reason and so reset to the CDO's (null) on
	 * every duplication; if this flag were a bare bool it would keep the ORIGINAL actor's
	 * "clean" value across that same copy, and the duplicate would serve null materials
	 * forever believing they were already resolved. Reflected the same way, it resets to
	 * the CDO's own default - true - in step with them.
	 */
	UPROPERTY(Transient) bool bResolvedContentDirty = true;

	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ResolvedSurfaceMaterialCache;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ResolvedApronMaterialCache;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ResolvedRubberMaterialCache;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ResolvedGhostMaterialCache;

	/** Indexed by RunwayMaterialSlot(Surface), exactly like FSurfaceSettings::RunwayMaterials -
	 *  a TArray rather than that struct's fixed C array because UPROPERTY reflection (what
	 *  keeps the garbage collector tracing these) has no fixed-array support for TObjectPtr. */
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInterface>> ResolvedRunwayMaterialsCache;

	/** Resolves SurfaceMaterial/ApronMaterial/RubberMaterial/GhostMaterial/the three runway
	 *  materials into the cache above if, and only if, bResolvedContentDirty - see the
	 *  cache's own comment. Called from MakeSurfaceSettings, which reads the cache after. */
	void RefreshResolvedContentCacheIfDirty();

	/**
	 * Constructor helper for the five CreateDefaultSubobject<UDynamicMeshComponent> blocks
	 * that used to repeat SetupAttachment plus three SetUsingAbsolute* calls each (issue #80).
	 * Factors only what is IDENTICAL across all five - the absolute-space setup every one of
	 * them needs for the same reason (see MeshComponent's own comment) - and leaves collision,
	 * shadow and visibility at the call site, because those genuinely differ per component
	 * (the road casts a shadow and has collision defaults the other four deliberately do not)
	 * and folding them in here would be a behaviour change dressed as a refactor.
	 *
	 * NAMES MUST STAY EXACTLY WHAT EACH CALLER PASSES: a saved level references its components
	 * by name (RoadMesh, RoadGhost, ApronMesh, HoldingPositionMarkings, RunwayMarkings).
	 */
	UDynamicMeshComponent* MakeSurfaceComponent(FName Name);

	/**
	 * How long an arrival sits at the stand before the engine stops, seconds.
	 *
	 * Not zero, and not a formality: an engine that stopped the instant the wheels did reads
	 * as a stall rather than a shutdown. Real enough to watch, short enough not to wait for.
	 * Private: only this actor's own DispatchArrival/DispatchAgent forwarders read it, to
	 * copy it into Traffic's calls - see UAirsideTraffic::DispatchArrival's own comment on
	 * why FRoadAgent cannot read it for itself.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside") double ShutdownPauseSeconds = 10.0;

	/**
	 * Everything the road LOOKS like - see URoadSurfacePresenter's own header for the
	 * pattern and why it is a UObject. CreateDefaultSubobject, not UPROPERTY(Instanced):
	 * Instanced exists to let an EDITABLE subobject property be swapped for a different
	 * instance or class in the Details panel and archetype-propagate across Blueprint
	 * children, none of which applies here - this is never exposed as EditAnywhere, is
	 * always exactly URoadSurfacePresenter, and every actor of this class needs its own
	 * (never shared, the way a CDO's own default subobject would be if nothing constructed
	 * a fresh one). CreateDefaultSubobject already gives it that, plus reachability through
	 * this actor's own UPROPERTY for the garbage collector, without inviting an edit this
	 * class must reject. Transient: nothing it holds is level content - the mesh components
	 * it draws into are saved on their own UPROPERTYs, and the ghost cache is exactly as
	 * disposable as it always was.
	 */
	UPROPERTY(Transient) TObjectPtr<URoadSurfacePresenter> Presenter;

	// THE PLOT PRESENTER AND ITS TWO COMPONENTS LIVED HERE until 2026-09-22 - see
	// AAirsideBuildingsActor, which owns them now, and OnTopologyRebuilt, which feeds it.

	/** Every graph mutator, query and undo step - see URoadEditFacade's own header. Same
	 *  CreateDefaultSubobject and Transient reasoning as Presenter. */
	UPROPERTY(Transient) TObjectPtr<URoadEditFacade> Facade;

	/** How many times RebuildMesh has run. Not a UPROPERTY - a session counter for
	 *  RebuildCountForTest, not state a save would ever need. */
	int32 RebuildCount = 0;

	/** How many of those calls ran the DERIVED-graph pass - guideline graph, anchor links,
	 *  plots, traffic - rather than skipping it for a Geometry-only change. See
	 *  TopologyRebuildCountForTest (issue #165): a Geometry notify bumps RebuildCount above
	 *  but not this, which is the whole measurement a drag-frame test needs. */
	int32 TopologyRebuildCount = 0;

	/** Agents and dispatch - see UAirsideTraffic's own header. Same CreateDefaultSubobject
	 *  and Transient reasoning as Presenter. */
	UPROPERTY(Transient) TObjectPtr<UAirsideTraffic> Traffic;

	/** See SetSimTimeScale. 1.0 is real time, which is what every caller before AirportOps got. */
	UPROPERTY(Transient) double SimTimeScale = 1.0;

	/**
	 * How fast the evened-out frame delta follows the real one, per frame. 1.0 disables the
	 * evening entirely and hands the model raw frame time, which is what tests that assert on
	 * an exact delta want.
	 *
	 * WHY THE DELTA IS EVENED AT ALL. The display presents frames on a fixed cadence - vsync
	 * holds each one for a whole number of refreshes - while DeltaSeconds is real measured
	 * wall-clock and jitters by several percent frame to frame. An agent advancing
	 * Speed x DeltaSeconds therefore covers a DIFFERENT distance in each equally-long display
	 * slot, and that is a velocity flicker, not a position error: measured from a play session
	 * on 2026-09-12, mean 5.1%, p95 16.8%, worst 36%.
	 *
	 * It matches every part of the report. The error is a fixed distance in world space, so it
	 * grows on screen as the camera closes in. It scales with speed - 1.64 uu a frame on a
	 * landing rollout against 0.14 uu in a tight turn - so a landing judders and a slow corner
	 * does not. And it is invisible to every test of the model, because the model is right:
	 * the aircraft really is where it says it is, at a time that is not evenly spaced.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	double DeltaSmoothingRate = 0.1;

	/**
	 * How far simulated time may run ahead of or behind the wall clock before the step is
	 * clamped to pull it back.
	 *
	 * A BOUND, NOT A BUDGET. Evening the delta means paying out the average rather than what
	 * the frame actually took, so a hitch leaves time owed and a fast frame pays it back. Left
	 * unbounded that is a slow drift between the agents and USimClock; bounded, it is a
	 * fraction of a second that closes itself - see FFrameDeltaSmoother::Advance's own
	 * "OWED-BANK RECOVERY" for the actual mechanism, and its world-free test
	 * (Airside.Present.FrameDeltaSmoother) for the numbers - and nobody can see.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.0"))
	double MaxOwedSeconds = 0.25;

	/** Builds the FSurfaceSettings RebuildMesh needs from this actor's own Resolve*
	 *  functions and level-authored tunables. One place, so a rebuild cannot read the
	 *  knobs into two different snapshots of itself. */
	URoadSurfacePresenter::FSurfaceSettings MakeSurfaceSettings();

	/**
	 * What OnChanged actually binds to (issue #165) - RebuildMesh() is a thin forwarder
	 * passing Topology, kept at its old name and signature because IRoadEditTarget, a
	 * UFUNCTION(CallInEditor) button, and every test in this plugin call it with no
	 * argument and expect a full rebuild. This is where Kind is read: Geometry runs the
	 * presenter's surface-only path and returns before the buildings (OnTopologyRebuilt)
	 * or Traffic are touched;
	 * Topology runs the whole pipeline exactly as RebuildMesh always has.
	 */
	void RebuildMeshForChange(EChangeKind Kind);

	/** The narrower FSurfaceSettings UpdateGhost/BuildGhostBuffers need - see its own
	 *  comment for why this is not MakeSurfaceSettings with most of it discarded. */
	URoadSurfacePresenter::FSurfaceSettings MakeGhostSurfaceSettings(ERoadKind Kind,
		int32 WidthIndex = INDEX_NONE);

public:
	/**
	 * The authored value if there is one, else the configured content default.
	 *
	 * READ-ONLY, and that is the whole point of them. These replaced a single
	 * ApplyContentDefaults that FILLED each property when it found it null - which looked
	 * harmless and was not: these are EditAnywhere properties on an actor that rebuilds at
	 * design time, so the fill landed on the level and was saved. An airport deliberately
	 * left on a single material acquired a material set it never asked for, permanently, and
	 * clearing it by hand only lasted until the next rebuild.
	 *
	 * It is the same defect ResolveProfile had, and this was the original of it. A resolver
	 * that writes what it resolves has turned a setting into a cache.
	 *
	 * PUBLIC because the authored value alone no longer answers "what will this actor use" -
	 * a test or a tool that read the property directly would see null and conclude nothing was
	 * configured, which was true of the raw field and false of the actor. MakeSurfaceSettings
	 * and URoadSurfacePresenter::Rebuild are the reason these are called at all now, in place
	 * of RebuildMesh reading the properties itself.
	 */
	UMaterialInterface* ResolveSurfaceMaterial() const;
	UMaterialInterface* ResolveApronMaterial() const;

	/** RubberMaterial, else the content default. Null is supported - see the property. */
	UMaterialInterface* ResolveRubberMaterial() const;

	/** TyreSmokeMaterial, else the content default. Null is supported: no smoke. */
	UMaterialInterface* ResolveTyreSmokeMaterial() const;
	UMaterialInterface* ResolveGhostMaterial() const;
	URoadMaterialSet*   ResolveMaterialSet() const;
	UEntityDefinition*  ResolveStandDefinition() const;

	/**
	 * What the fuel depot tool places: the authored value if there is one, else the
	 * configured content default. Null is a supported state - PlaceEntity refuses and names
	 * the asset that is missing.
	 */
	UEntityDefinition*  ResolveFuelDepotDefinition() const;

	/**
	 * ResolveStandDefinition or ResolveFuelDepotDefinition, by kind.
	 *
	 * THE ONE PLACE the mapping lives, so a tool's preview and the facade's placement cannot
	 * pick differently - which is the drift GetStandDefinition's own comment has always
	 * warned about, and which only becomes possible once there are two kinds.
	 */
	UEntityDefinition*  ResolveEntityDefinition(EPlaceableEntity Kind) const;

	/**
	 * The service-road cross-section: the authored value if there is one, else the configured
	 * content default. Null is a SUPPORTED state - the road tool refuses and says so, rather
	 * than laying a taxiway under the name of a road.
	 *
	 * Const, unlike ResolveProfile, because there is nothing to cache: this never falls back
	 * to a transient profile, for the reason ServiceRoadProfile's own comment gives.
	 */
	URoadProfile*       ResolveServiceRoadProfile() const;

	/**
	 * A runway's pavement material by its surface fact, from the content set only - there
	 * is no per-actor override, because a runway's look is a project-wide fact like its
	 * markings. Null when the content set names none: the presenter then falls back to
	 * the surface material, and the runway draws as a road.
	 */
	UMaterialInterface* ResolveRunwayMaterial(ERunwaySurface Surface) const;

	/**
	 * AUTHORED INPUT, READ AND NEVER WRITTEN save for the on-demand fallback cache - see
	 * the .cpp. Public (moved from private by issue #32): URoadEditFacade::ConnectNodes and
	 * ::DeleteNode call this directly, in place of the bare member access they used when
	 * they were part of this class. Still non-const, because it lazily fills RuntimeProfile
	 * - see ResolveProfileForTest for the const-preserving path a test needs instead.
	 */
	URoadProfile* ResolveProfile();

	/**
	 * ResolveProfile, for the test that guards it. Not for production use.
	 *
	 * ResolveProfile is non-const because it caches into RuntimeProfile, and
	 * Airside.Present.AuthoredPropertiesUntouched has to be able to ask what the actor would
	 * use without being given the write access that whole test exists to forbid.
	 */
	const URoadProfile* ResolveProfileForTest() { return ResolveProfile(); }

	// SurfaceTriangleCountForTest/RunwayMarkingTriangleCountForTest/EffectiveMaterialSetForTest
	// deleted (code review on issue #80's PR): they forwarded to Presenter with nothing added,
	// and GetPresenter() above exists precisely so a test can ask the presenter itself instead
	// of the actor growing one forwarder per presenter query. Callers (MeshFreshnessTest.cpp,
	// RunwaySurfaceTest.cpp) now call Actor->GetPresenter()->SurfaceTriangleCountForTest() etc.
	// directly - see URoadSurfacePresenter's own header for those three.

	/**
	 * How many times RebuildMesh has run, for Airside.Present.MeshRebuildsOnFacadeChange.
	 *
	 * A count survives where a triangle-count comparison cannot: MoveNode's own rebuild can
	 * leave the triangle count exactly as it was (same segment, same profile, a shifted
	 * vertex), so a test asserting notification for it needs a signal a shape change is not
	 * guaranteed to give. Kept on the ACTOR, not the presenter: it counts calls to
	 * ARoadNetworkActor::RebuildMesh itself (the OnChanged handler), not presenter-internal
	 * rebuild work, so a future presenter-side cache hit that skips real work would not
	 * silently break this count's meaning.
	 */
	int32 RebuildCountForTest() const { return RebuildCount; }

	/**
	 * How many of those rebuilds ran the guideline graph / anchor links / plots / traffic
	 * pass, for Airside.Present.DragNotifiesGeometryOnly (issue #165).
	 *
	 * SEPARATE FROM RebuildCount, on purpose: a drag of N MoveNode frames bumps RebuildCount
	 * N times (the surface must repaint every frame) but this only once, at
	 * EndInteractiveEdit - which is exactly the claim "a drag frame does not re-derive the
	 * graph" and RebuildCount alone cannot state.
	 */
	int32 TopologyRebuildCountForTest() const { return TopologyRebuildCount; }

	/** The newest agent's Phase, for the same test - see UAirsideTraffic::
	 *  LastAgentPhaseForTest for why Gone stands in for "no agent". */
	EAgentPhase LastAgentPhaseForTest() const;

	/** The newest agent's own taxi speed cap, for the same test. Forwards to Traffic. */
	double LastAgentTaxiSpeedCapForTest() const;

	/** The stand definition this actor would use, for the same test. */
	UEntityDefinition* ResolveStandDefinitionForTest() const { return ResolveStandDefinition(); }

	/** Whose subobject the facade / presenter is, for Airside.Present.DuplicatedActorOwnsItsSubobjects. */
	UObject* FacadeOuterForTest() const;
	UObject* PresenterOuterForTest() const;

	/**
	 * The presenter's own LayerComponents[Layer], for Airside.Present.NetworkActor.
	 *
	 * Compared there against THIS actor's named fields (MeshComponent, GhostComponent, ...) -
	 * the identity check the constructor's ESurfaceLayer indexing has no other test for.
	 * Every rebuild-triggered test reads a layer's component back out through the presenter's
	 * own table, so a Road<->Ghost or HoldingPaint<->RunwayPaint swap in the constructor's
	 * indexing would still rebuild something, correctly, at the swapped slot, and pass -
	 * only comparing against the actor's independently-named fields catches that.
	 */
	UDynamicMeshComponent* LayerComponentForTest(ESurfaceLayer Layer) const
	{
		return GetPresenter() != nullptr ? GetPresenter()->GetLayerComponentForTest(Layer) : nullptr;
	}

public:
	/** Absolute world-space Z of the road surface, in uu. Not relative to the actor:
	 *  the mesh builder emits world-space XY at this Z, and MeshComponent is set to use
	 *  absolute location/rotation/scale (see the constructor) so those coordinates are
	 *  not transformed again by the actor's own placement. */
	UPROPERTY(EditAnywhere, Category = "Airside") double SurfaceZ = 10.0;

	/** Quads along each segment. 1 is right for straight segments. */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1")) int32 RibbonSegments = 1;

	/**
	 * World units per texture tile for the asphalt. Lower means the texture repeats more
	 * often, so more visible grain across the road.
	 *
	 * At the default 512 a 200 uu road shows less than half of one tile across its whole
	 * width, which magnifies the texture until it reads as flat colour. Roughly a fifth
	 * of the road's width is a sane starting point.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1.0")) double TexelsPerUnit = 512.0;

	/**
	 * Draw every triangle the builder produced as debug lines.
	 *
	 * Ground truth for "the mesh is correct but nothing renders": these come from the
	 * same buffers the component is handed, but reach the screen by a completely separate
	 * path, so whatever shows here is the geometry itself - independent of materials,
	 * bounds, clip planes and the scene proxy.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside") bool bDebugDrawMesh = false;

	/** How long the debug wireframe survives, in seconds. */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.0")) double DebugDrawSeconds = 30.0;
};
