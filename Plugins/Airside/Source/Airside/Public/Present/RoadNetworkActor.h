#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/RoadHandles.h"
#include "Entities/EntityDefinition.h"
#include "Present/RoadSurfacePresenter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadSnap.h"
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

	// --- Agents ----------------------------------------------------------------------
	//
	// Runtime only, and owned by Traffic rather than by this actor or by URoadNetwork - see
	// UAirsideTraffic's class comment for why an agent belongs to neither.

	/** Lands an aircraft on the runway nearest a point and taxis it to a stand. Forwards to
	 *  Traffic - see UAirsideTraffic::DispatchArrival for the arm/spawn/log this actor used
	 *  to do itself, and Model/ArrivalPlanner for which runway, exit and stand are chosen. */
	bool DispatchArrival(const FVector2D& Near, const FAirframe& Airframe);

	/** Sends one agent along a plan, spawning the cube that shows it. Forwards to Traffic. */
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe) override;

	/** Removes every agent and its cube. Forwards to Traffic. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Airside")
	void ClearAgents();

	/** How many agents are currently under way or parked at their destination. Forwards to
	 *  Traffic. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 GetAgentCount() const;

	/** The most recently dispatched agent's actor, or null. Forwards to Traffic. */
	ARoadAgentActor* GetNewestAgent() const;

	/** Route between two guideline nodes over the network this actor owns. Forwards to the
	 *  facade, so a tool, a Blueprint and the HUD all ask the same question of the same
	 *  graph rather than three of them reaching past it. */
	virtual FRoutePlan FindRoute(
		FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan) const override;

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

	/** Add a node at a world-space XY position. Returns its index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 PlaceNode(FVector2D Where) override;

	/** Join two placed nodes with a straight segment. Returns false, and logs, if it refused. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex) override;

	/** Link two GUIDELINE nodes by hand. Returns the new edge's index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) override;

	/** Lays a runway from From to To in one edit, with its own profile. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile) override;

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

	/** Remove a HAND-AUTHORED guideline edge. Refuses a derived one. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	virtual bool DisconnectGuideline(int32 EdgeIndex) override;

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

	/** Open an edit that spans frames, for a drag. */
	virtual void BeginInteractiveEdit(const FString& Label) override;

	/** Close it. bKeep false abandons the snapshot, leaving no undo step. */
	virtual void EndInteractiveEdit(bool bKeep) override;

	/** What deleting NodeIndex would do, without doing any of it. For the overlay. */
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const override;

	/**
	 * Limits the deletion plan judges its rejoins against. Set from the build tool.
	 *
	 * Stays a plain field on the actor, not moved to the facade: RoadBuildController writes
	 * it directly every frame (Target->PlacementLimits = ...) on the concrete actor type, so
	 * moving it would mean either breaking that write or adding a forwarding setter for a
	 * struct assignment - more machinery than a runtime-only tuning knob is worth. The facade
	 * reads it back through its owning actor; see URoadEditFacade::MoveNode.
	 */
	FRoadPlacementLimits PlacementLimits;

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
	virtual int32 PlaceStand(FVector2D Where, double Heading) override;

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
	 * IRoadEditTarget accessor for StandDefinition - RESOLVED, via ResolveStandDefinition(),
	 * the same as PlaceStand places from: preview and placement must resolve the same
	 * object, or a stand's ghost and the stand PlaceStand actually drops can disagree.
	 */
	virtual const UEntityDefinition* GetStandDefinition() const override
	{
		return ResolveStandDefinition();
	}

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

	/** Show the segment a click would build, as real solved pavement. Forwards to Presenter
	 *  with a FSurfaceSettings built the same way RebuildMesh's is. */
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid) override;

	/**
	 * The ghost's triangles, without touching a component, a material or a renderer.
	 *
	 * Public and separated from UpdateGhost so the one property this whole mechanism
	 * rests on can be asserted in a test with no World: building a preview must leave the
	 * REAL network bitwise unchanged. Forwards to Presenter.
	 */
	bool BuildGhostBuffers(int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers);

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
	double FallbackWidth = 2300.0;

	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "0.0"))
	double FallbackFilletRadius = 1500.0;

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

	/** Snapshots of the graph before each edit. See URoadEditHistory for why Memento
	 *  rather than the Command layer design spec 7.3 specifies. Stays on the actor for the
	 *  same saved-with-the-level reason as Network. */
	UPROPERTY() TObjectPtr<URoadEditHistory> History;

private:
	/** Profile made on demand when none is authored. Transient so it is never saved. */
	UPROPERTY(Transient) TObjectPtr<URoadProfile> RuntimeProfile;

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

	/** Every graph mutator, query and undo step - see URoadEditFacade's own header. Same
	 *  CreateDefaultSubobject and Transient reasoning as Presenter. */
	UPROPERTY(Transient) TObjectPtr<URoadEditFacade> Facade;

	/** Agents and dispatch - see UAirsideTraffic's own header. Same CreateDefaultSubobject
	 *  and Transient reasoning as Presenter. */
	UPROPERTY(Transient) TObjectPtr<UAirsideTraffic> Traffic;

	/** Builds the FSurfaceSettings RebuildMesh needs from this actor's own Resolve*
	 *  functions and level-authored tunables. One place, so a rebuild cannot read the
	 *  knobs into two different snapshots of itself. */
	URoadSurfacePresenter::FSurfaceSettings MakeSurfaceSettings();

	/** The narrower FSurfaceSettings UpdateGhost/BuildGhostBuffers need - see its own
	 *  comment for why this is not MakeSurfaceSettings with most of it discarded. */
	URoadSurfacePresenter::FSurfaceSettings MakeGhostSurfaceSettings();

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
	UMaterialInterface* ResolveGhostMaterial() const;
	URoadMaterialSet*   ResolveMaterialSet() const;
	UEntityDefinition*  ResolveStandDefinition() const;

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

	/**
	 * Triangles currently in the road surface, for Airside.Present.MeshIsFreshAfterLoad.
	 * Forwards to Presenter, which is what actually holds MeshComponent's built mesh.
	 */
	int32 SurfaceTriangleCountForTest() const;

	/** Agents alive right now, for Airside.Present.ArrivalDispatch. Forwards to Traffic. */
	int32 AgentCountForTest() const;

	/** The stand definition this actor would use, for the same test. */
	UEntityDefinition* ResolveStandDefinitionForTest() const { return ResolveStandDefinition(); }

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
