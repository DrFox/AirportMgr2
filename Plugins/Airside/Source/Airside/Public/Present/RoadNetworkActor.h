#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "Model/RouteFollower.h"
#include "Model/TakeoffRun.h"
#include "Entities/EntityDefinition.h"
#include "Tool/RoadEditHistory.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadSnap.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class URoadProfile;
class ARoadAgentActor;
class UDynamicMeshComponent;
class UMaterialInstanceDynamic;
class FRoadMeshBuilder;
struct FRoadSolveResult;
class UMaterialInterface;
class URoadMaterialSet;

namespace UE::Geometry { class FDynamicMesh3; }

/** Pushes finished buffers into a UDynamicMeshComponent. */
class AIRSIDE_API FDynamicMeshSink : public IRoadMeshSink
{
public:
	/**
	 * InMaterials null keeps the single-material path: one SetMaterial(0, InMaterial) and
	 * no material-ID attribute, exactly as before per-band materials existed.
	 */
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent, UMaterialInterface* InMaterial = nullptr,
		bool bInUseConstantVertexColour = true, const URoadMaterialSet* InMaterials = nullptr)
		: Component(InComponent), Material(InMaterial)
		, bUseConstantVertexColour(bInUseConstantVertexColour), Materials(InMaterials) {}
	virtual void Accept(const FRoadMeshBuffers& Buffers) override;

	/**
	 * Copy the buffers' UV, colour and material-id channels onto an already-populated mesh.
	 *
	 * Static and public so it can be tested without a component, a world or a renderer.
	 * The buffers being correct says nothing about what the component receives, and that
	 * gap is precisely where slice 2a's invisible surface hid.
	 */
	static void PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);

	/**
	 * Convert whole buffers into a mesh - vertices, triangles, UVs and material ids.
	 * Returns the number of triangles FDynamicMesh3 refused.
	 *
	 * Static and public for the same reason as PopulateAttributes, and it carries the one
	 * correspondence in this file that is NOT the identity: FDynamicMesh3::AppendTriangle
	 * REFUSES non-manifold and duplicate triangles, so one refusal shifts every later
	 * triangle's id away from its buffer index. Indexing MaterialIDs by mesh triangle id
	 * would then re-skin the entire mesh downstream of the first refusal, silently. That
	 * is the index-parallel defect this codebase has already paid for once, in
	 * FEntityInstance::ResolvedAnchors; here the mapping is recorded as the triangles are
	 * appended, where it is known, rather than assumed afterwards.
	 */
	static int32 BuildMesh(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);

private:
	// Raw, non-owning pointers: the sink owns and GC-protects neither, and must not
	// outlive either. Both current call sites are stack-scoped inside a single function,
	// so this is safe today; a preview sink that lives across frames will not be.
	UDynamicMeshComponent* Component = nullptr;
	UMaterialInterface* Material = nullptr;

	/** Independent of Material by design - see ARoadNetworkActor::bUseConstantVertexColour. */
	bool bUseConstantVertexColour = true;

	/** Null means the single-material path. Non-owning, like Component and Material. */
	const URoadMaterialSet* Materials = nullptr;
};

/**
 * One thing driving one route, plus the cube standing where it is.
 *
 * The follower is the model and the actor is the view, which is why they are two fields
 * rather than one class: everything about whether the route is walked correctly is decided
 * in FRouteFollower, with no world involved and no actor to spawn.
 *
 * Runtime only. These are NOT saved with the level - see ARoadNetworkActor::Agents.
 */
USTRUCT()
struct AIRSIDE_API FRoadAgent
{
	GENERATED_BODY()

	UPROPERTY() FRouteFollower Follower;

	UPROPERTY() TObjectPtr<ARoadAgentActor> View = nullptr;

	/**
	 * The departure this agent flies once the taxi is done, if its route ended on a runway.
	 *
	 * A SECOND MOTION PHASE rather than a mode inside the follower - see FTakeoffRun. The
	 * agent owns which of the two is driving it, so neither has to know the other exists.
	 */
	UPROPERTY() FTakeoffRun Departure;

	/** True once the taxi has finished and the departure has taken over. */
	UPROPERTY() bool bDeparting = false;

	/** Armed at dispatch when the route's goal was a runway threshold. */
	UPROPERTY() bool bDepartOnArrival = false;

	/**
	 * The engine is turning. NOT the same question as whether the aircraft is moving.
	 *
	 * This was inferred from movement - the propeller stopped whenever the aircraft did -
	 * which is wrong at both ends. An aircraft holding short with its engine idling is the
	 * commonest thing on an airport, and one that has actually shut down could not be
	 * expressed at all.
	 *
	 * True from dispatch until the agent goes. A shutdown at the stand is a turnaround state
	 * and belongs with the rest of that when it exists; what matters here is that the answer
	 * is STATE rather than a guess made from the speed.
	 */
	UPROPERTY() bool bEngineRunning = false;

	/**
	 * What to show for this agent right now: where it is, and what it is doing.
	 *
	 * ON THE AGENT rather than in ARoadNetworkActor::Tick, which is where it used to be
	 * assembled. Buried in a tick that needs a world, "is the engine running" was a line
	 * nothing could test - and it was wrong for as long as it existed. Here it is a pure
	 * function of the agent's own state, so Airside.Present.AgentMotion can ask it directly.
	 */
	FAgentMotion DescribeMotion(const FVector2D& At, double Heading,
		double Altitude = 0.0, double PitchDegrees = 0.0) const;

	/** Where the roll starts, and which way. Unused unless bDepartOnArrival. */
	UPROPERTY() FVector2D DepartureThreshold = FVector2D::ZeroVector;
	UPROPERTY() FVector2D DepartureDirection = FVector2D::ZeroVector;
	UPROPERTY() double DepartureRunwayLength = 0.0;
	UPROPERTY() FClimbPerformance DepartureClimb;
};

/** Owns a road network and renders it as one batched dynamic mesh. */
UCLASS()
class AIRSIDE_API ARoadNetworkActor : public AActor
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
	 * value with no invalidation is stale by definition. It stayed stale until something
	 * rebuilt for an unrelated reason, at which point roads the user had drawn in an
	 * earlier session visibly changed width and material. See
	 * Airside.Present.MeshIsFreshAfterLoad, and the measurement: 276 triangles loaded from
	 * the level against 194 the model produced.
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
	 * origin - and a sprite there reads as a node the build tool drew at (0,0), which is
	 * precisely the false picture the editor mode has already produced twice. The
	 * constructor clears the flag; this catches any component another path attached.
	 */
	virtual void PostRegisterAllComponents() override;

	/**
	 * The history an edit should snapshot into, or NULL when the editor owns undo.
	 *
	 * In an editor world the transaction system already serialises the network on Modify()
	 * and restores it on Ctrl+Z - which is the same job the Memento does. Running both
	 * would leave two stacks disagreeing about one graph, and the editor's is the one a
	 * user will reach for. So in the editor this returns null and FRoadEditScope becomes a
	 * no-op; at runtime, where there is no transaction system, it returns the history.
	 */
	URoadEditHistory* HistoryForEdit();

	/** Solve every node, build the mesh, and push it to the component. */
	UFUNCTION(CallInEditor, Category = "Airside")
	void RebuildMesh();

	virtual void Tick(float DeltaSeconds) override;

	/** True outside a game world, so dispatched agents move in the editor viewport too. */
	virtual bool ShouldTickIfViewportsOnly() const override;

	// --- Agents ----------------------------------------------------------------------
	//
	// Runtime only, and deliberately not part of URoadNetwork. An agent is a thing part
	// way through a journey, not a fact about the airport: putting them in the network
	// would snapshot them into every undo Memento and serialise them into the saved level,
	// so re-opening a map would restore half-driven cubes that no longer have a route.

	/**
	 * Sends one agent along a plan, spawning the cube that shows it. False if it cannot.
	 *
	 * Works in an editor world as well as in play: the cubes are spawned RF_Transient and
	 * so are never saved, and the build tools this is driven from are used at design time.
	 *
	 * Takes the AIRFRAME's ground performance rather than a bare speed. How fast a thing
	 * taxis and how fast it can be turned are both facts about the aeroplane, and splitting
	 * them across two arguments invited a caller to pass one and default the other.
	 */
	bool DispatchAgent(const FRoutePlan& Plan, const FGroundPerformance& Ground,
		const FClimbPerformance& Climb = FClimbPerformance());

	/** Removes every agent and its cube. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Airside")
	void ClearAgents();

	/** How many agents are currently under way or parked at their destination. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 GetAgentCount() const { return Agents.Num(); }

	/**
	 * The most recently dispatched agent's actor, or null when nothing is under way.
	 *
	 * The NEWEST rather than the nearest or the first: the one you just sent is the one you
	 * want to watch, and any other rule makes "follow it" mean something different depending
	 * on what else happens to be taxiing.
	 */
	ARoadAgentActor* GetNewestAgent() const
	{
		return Agents.Num() > 0 ? Agents.Last().View.Get() : nullptr;
	}

	/**
	 * Route between two guideline nodes over the network this actor owns.
	 *
	 * On the facade so a tool, a Blueprint and the HUD all ask the same question of the
	 * same graph rather than three of them reaching past it.
	 */
	FRoutePlan FindRoute(
		FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan) const;

	// --- Runtime graph facade -------------------------------------------------------
	//
	// The player builds the network while the game runs, so these are the entry points
	// a build tool drives. They are deliberately on the actor rather than on
	// URoadNetwork: Model/RoadNetwork.h reserves the mutators for IRoadCommand from
	// Slice 3 onward, and when that lands these bodies start emitting commands while
	// every caller - Blueprint included - stays exactly as it is. Placing them here is
	// what keeps that swap from being a breaking change.
	//
	// Node identity crosses the boundary as a plain int32 slot index rather than
	// FRoadNodeId, because a USTRUCT handle does not round-trip cleanly through
	// Blueprint and the index is unambiguous within one network.

	/** Add a node at a world-space XY position. Returns its index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 PlaceNode(FVector2D Where);

	/** Join two placed nodes with a straight segment. Returns false, and logs, if it refused. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool ConnectNodes(int32 FromIndex, int32 ToIndex);

	/**
	 * Link two GUIDELINE nodes by hand. Returns the new edge's index, or INDEX_NONE.
	 *
	 * The routing graph is derived from pavement and can therefore only connect what
	 * pavement connects. This is how a connection is made that no road expresses - across
	 * an apron, or to a stand whose lead-in found nothing.
	 *
	 * The edge is bDerived == false, so the builder leaves it alone, and carries both
	 * endpoints' identities so every later rebuild re-attaches it. Both of those matter:
	 * without the second it survives every rebuild connected to nothing.
	 */

	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex);

	/**
	 * Lays a runway from From to To in one edit, with its own profile.
	 *
	 * Separate from PlaceNode plus ConnectNodes for two reasons. It is ONE undo step, which is
	 * what a player means by "place a runway"; and it takes the profile explicitly, because a
	 * runway's cross-section is not the network's default and must never fall back to it - a
	 * runway that quietly became a taxiway would keep its shape on screen and lose its
	 * continuity at every exit.
	 *
	 * Straight by construction: one segment, two nodes, no control point. Refuses a runway
	 * shorter than MinimumRunwayLength, because a strip too short to take off from is a
	 * mis-click rather than an intention.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile);

	/**
	 * The shortest thing that may be called a runway, in uu. 500 m.
	 *
	 * Not an aviation rule - real minima depend on the aircraft - but a floor that separates
	 * a runway from a slip of the mouse. A Meridian needs about 700 m at sea level.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1.0"))
	double MinimumRunwayLength = 50000.0;

	/**
	 * Remove a HAND-AUTHORED guideline edge. Refuses a derived one.
	 *
	 * Refuses rather than obeys, because the next rebuild would put a derived edge straight
	 * back - which reads as the tool ignoring the click.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool DisconnectGuideline(int32 EdgeIndex);

	/**
	 * Index of the nearest live node within Radius of Where, or INDEX_NONE.
	 *
	 * A crude stand-in for the snap chain of section 7.4. Something has to let a click
	 * reattach to an existing node, or every road drawn would be disconnected from the
	 * last and no junction could ever be authored.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 FindNodeNear(FVector2D Where, double Radius) const;

	/**
	 * Replace a live segment with two, meeting at a new node placed at At.
	 *
	 * Returns the new node's index, or INDEX_NONE if it refused. This is what makes a
	 * T-junction authorable: without it a junction can only ever form where a node was
	 * already placed, so running a taxiway into a road you have already drawn is
	 * impossible.
	 *
	 * The original segment's handle is DEAD afterwards - the segment is removed, not
	 * reshaped, because its endpoints define its identity and both of them change.
	 * Anything holding that handle must re-resolve. Both replacements inherit the
	 * original's profile.
	 *
	 * At is taken as given rather than projected onto the segment: the snap chain has
	 * already found the point, and re-deriving it here would let the two disagree about
	 * where the split is. A caller passing a point off the segment gets a kink, which is
	 * a caller error and not something to silently correct.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 SplitSegment(int32 SegmentIndex, FVector2D At);

	/**
	 * Remove a node, rejoining the roads it would otherwise strand. See RoadHeal.h.
	 *
	 * Refuses WHOLE, changing nothing, if any rejoin would break a placement rule - a
	 * junction can therefore become undeletable, and the way out is to delete its arms
	 * individually until it is bare. Nothing is ever moved or lost unexpectedly, which is
	 * the trade that choice buys.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool DeleteNode(int32 NodeIndex);

	/**
	 * Remove one segment.
	 *
	 * Either endpoint left holding no road at all goes with it. That is cleanup rather
	 * than deletion: a node with no segments carries no geometry, so removing it destroys
	 * nothing - and leaving it behind is just litter on the map.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool DeleteSegment(int32 SegmentIndex);

	/** Slot indices of the segments that deleting NodeIndex would take with it. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	TArray<int32> SegmentsIncidentTo(int32 NodeIndex) const;

	/**
	 * Move a node, dragging its roads with it.
	 *
	 * Refused if it would pull any of its roads under MinSegmentLength - so a node being
	 * dragged simply stops following the cursor rather than producing a segment the solver
	 * cannot trim. Turn angles are NOT checked: a node between two roads can legitimately
	 * be dragged through any angle, and refusing mid-drag would read as the node sticking.
	 *
	 * Undoable on its own, and joins an open interactive edit when there is one - so a
	 * whole drag is one undo step rather than one per frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool MoveNode(int32 NodeIndex, FVector2D To);

	/**
	 * Open an edit that spans frames, for a drag.
	 *
	 * Everything done until EndInteractiveEdit becomes one undo step. Without this a drag
	 * would push a snapshot per frame and undo would crawl back along the path the mouse
	 * took.
	 */
	void BeginInteractiveEdit(const FString& Label);

	/** Close it. bKeep false abandons the snapshot, leaving no undo step. */
	void EndInteractiveEdit(bool bKeep);

	/** What deleting NodeIndex would do, without doing any of it. For the overlay. */
	FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const;

	/** Limits the deletion plan judges its rejoins against. Set from the build tool. */
	FRoadPlacementLimits PlacementLimits;

	/** Both endpoints of a live segment, on the road plane. False if it is not live. */
	bool GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const;

	// --- Aprons -----------------------------------------------------------------------

	/**
	 * Add a polygon of pavement. Returns its slot index, or INDEX_NONE if refused.
	 *
	 * Refuses an outline of fewer than three corners, or one that crosses itself - the
	 * triangulator's contract is a SIMPLE polygon, and a figure-eight fed to it produces
	 * triangles that overlap rather than an error.
	 *
	 * Winding is corrected rather than refused: FApronSurface asks for counter-clockwise,
	 * the shoelace sign says which way round this is, and reversing a clockwise outline is
	 * an answer where refusing would only be a complaint.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 AddApron(const TArray<FVector2D>& Outline);

	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool DeleteApron(int32 ApronIndex);

	/** The topmost apron containing a point, or INDEX_NONE. For picking. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 FindApronAt(FVector2D Where) const;

	// --- Stands -----------------------------------------------------------------------

	/**
	 * Place a stand, facing Heading in radians. Returns its slot index, or INDEX_NONE.
	 *
	 * Every anchor the definition declares resolves to a NON-DERIVED guideline node, so the
	 * guideline builder's orphan sweep leaves them alone and the handles survive every
	 * taxiway edit. That is what makes "drive to stand 12's fuel position" an ordinary path
	 * query rather than a lookup that goes stale.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 PlaceStand(FVector2D Where, double Heading);

	/**
	 * Remove a placed entity, and the anchor nodes it owns.
	 *
	 * RemoveGuidelineNode cascades, so this also removes any guideline drawn INTO the
	 * stand - a lead-in to a stand that is gone leads nowhere. Destructive and undoable.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool DeleteEntity(int32 EntityIndex);

	/** Nearest placed entity within Radius of a point, or INDEX_NONE. For picking. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	int32 FindEntityAt(FVector2D Where, double Radius) const;

	/**
	 * The stand layout new stands are placed from. Defaults to DA_Stand_CodeC.
	 *
	 * A Flyweight: every stand shares one definition and carries only its own pose, which
	 * is the whole reason anchors live on the definition rather than on the instance.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Stands")
	TObjectPtr<UEntityDefinition> StandDefinition;

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
	 * Show the segment a click would build, as real solved pavement.
	 *
	 * Built on a DUPLICATE of the network, never the live one. FRoadNetworkSolver::SolveAll
	 * takes a non-const network and writes trim distances and cut vertices INTO it, so
	 * solving a hypothetical segment against the real graph would leave the real road's
	 * stored geometry describing a road nobody built - and it would only show once
	 * something forced a rebuild.
	 *
	 * A Segment snap is split on the copy for real, because a split turns one road into a
	 * three-arm junction and nothing short of performing it shows that junction's shape.
	 *
	 * bValid drives the material's ValidityBlend only. Validity is a parameter rather than
	 * a mesh variant, so turning a drag red regenerates no geometry at all.
	 */
	void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid);

	/**
	 * The ghost's triangles, without touching a component, a material or a renderer.
	 *
	 * Public and separated from UpdateGhost so the one property this whole mechanism
	 * rests on can be asserted in a test with no World: building a preview must leave the
	 * REAL network bitwise unchanged. That failure is otherwise invisible - the ghost
	 * looks right either way, and the damage only surfaces later as a road whose stored
	 * cut vertices describe a segment nobody built.
	 */
	bool BuildGhostBuffers(int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers);

	/** Hide the preview and forget what it was showing. */
	void HideGhost();

	/** A live node's handle from its slot index, or false if it is not live. */
	bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const;

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
	 * working.
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
	/**
	 * Name -> material for the road surface's profile bands. Null renders exactly as
	 * before: one material, every triangle id 0.
	 *
	 * A DataAsset rather than a table edited here, because this actor lives in a level
	 * that is deliberately never saved - see URoadMaterialSet.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TObjectPtr<URoadMaterialSet> MaterialSet;

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
	 * screen by a completely separate route. If these lines land where the outline was
	 * drawn and the concrete does not, the fault is in the component, the material or the
	 * view - never the geometry. If the lines are wrong too, every conclusion drawn from
	 * triangle counts so far needs revisiting.
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
	 * Height the apron surface is actually built at.
	 *
	 * ApronZOffset is a MAXIMUM, not a fixed drop: the apron never descends more than
	 * halfway from the road to the ground plane. A fixed drop silently assumes the road has
	 * headroom, and with SurfaceZ at 1 a 4 uu drop put the concrete at Z = -3 - rendering
	 * correctly, normals up, material bound, and buried under the ground where nothing
	 * about it looked wrong.
	 *
	 * Halfway rather than clamped at zero because zero is where the ground is: an apron
	 * pinned exactly to it would z-fight with the terrain instead of vanishing under it,
	 * which trades one silent failure for another.
	 *
	 * Public and shared so the mesh, the log and the tests cannot each compute it their
	 * own way and disagree.
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
	 *  through the facade above, so every change stays undoable. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside") TObjectPtr<URoadNetwork> Network;

	/** Snapshots of the graph before each edit. See URoadEditHistory for why Memento
	 *  rather than the Command layer design spec 7.3 specifies. */
	UPROPERTY() TObjectPtr<URoadEditHistory> History;

private:
	/** Profile made on demand when none is authored. Transient so it is never saved. */
	UPROPERTY(Transient) TObjectPtr<URoadProfile> RuntimeProfile;

	/** The hypothetical graph the ghost is solved against. Rebuilt whenever the drag moves. */
	UPROPERTY(Transient) TObjectPtr<URoadNetwork> GhostNetwork;

	/**
	 * Transient, and that is the whole point: agents never reach disk.
	 *
	 * UPROPERTY regardless, because View is a UObject pointer and an agent that the
	 * garbage collector cannot see is an agent whose cube is collected out from under it.
	 */
	UPROPERTY(Transient) TArray<FRoadAgent> Agents;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> GhostMID;

	// What the ghost currently shows. A drag holds still for most frames, and rebuilding
	// an unchanged preview means duplicating the network and re-solving it every frame for
	// an identical result.
	int32 LastGhostFrom = INDEX_NONE;
	FVector2D LastGhostTo = FVector2D::ZeroVector;
	ERoadSnapKind LastGhostKind = ERoadSnapKind::Free;
	bool bLastGhostValid = true;
	bool bGhostVisible = false;

	/** The authored profile if there is one, otherwise the on-demand fallback. */
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
	 * configured, which was true of the raw field and false of the actor.
	 */
public:
	UMaterialInterface* ResolveSurfaceMaterial() const;
	UMaterialInterface* ResolveApronMaterial() const;
	UMaterialInterface* ResolveGhostMaterial() const;
	URoadMaterialSet*   ResolveMaterialSet() const;
	UEntityDefinition*  ResolveStandDefinition() const;

	/**
	 * ResolveProfile, for the test that guards it. Not for production use.
	 *
	 * ResolveProfile is private and non-const because it caches into RuntimeProfile, and
	 * Airside.Present.AuthoredPropertiesUntouched has to be able to ask what the actor would
	 * use without being given the write access that whole test exists to forbid.
	 */
	const URoadProfile* ResolveProfileForTest() { return ResolveProfile(); }

	/**
	 * Triangles currently in the road surface, for Airside.Present.MeshIsFreshAfterLoad.
	 *
	 * The mesh is what the user SEES, and the whole of that test is that seeing and
	 * modelling agree. Counting triangles is the cheapest measure that moves when the
	 * surface does, and it needs no GeometryFramework dependency in the test module.
	 */
	int32 SurfaceTriangleCountForTest() const;

private:

	URoadProfile* ResolveProfile();

	/** Network, creating it on first use. Nothing else in the project makes one yet. */
	URoadNetwork& EnsureNetwork();

	/** Undo history, created on first use and kept in step with MaxUndoDepth. */
	URoadEditHistory& EnsureHistory();

	/** Rebuild the apron surface. Separate from the roads, which share none of it. */
	void RebuildAprons();

	/** A live segment's handle from its slot index. See MakeLiveNodeId. */
	bool MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const;

	/**
	 * The split surgery itself, against any network.
	 *
	 * Shared by the real edit and the ghost deliberately. Two implementations of the same
	 * surgery is precisely how a preview comes to show something the click will not do,
	 * and that failure is invisible - the ghost looks plausible either way.
	 */
	static FRoadNodeId SplitSegmentIn(URoadNetwork& Net, FRoadSegmentId Doomed, const FVector2D& At);

	/** Append a solved node's fan to Builder, if that node solved at all. */
	void AddGhostJunction(FRoadMeshBuilder& Builder, const FRoadSolveResult& Solved, int32 NodeIndex) const;

	/** The ghost's material instance, made on first use. Null if GhostMaterial is unset. */
	UMaterialInstanceDynamic* GhostMaterialInstance();

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
