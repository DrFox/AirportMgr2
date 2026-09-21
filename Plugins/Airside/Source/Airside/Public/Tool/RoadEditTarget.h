#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RouteSearch.h"
#include "Entities/EntityDefinition.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadSnap.h"

class URoadNetwork;
class IBuildPurse;
class URoadProfile;
class UGroundTraffic;
class UEntityDefinition;

/**
 * What a graph change notification is ABOUT - issue #165, extended to three kinds by issue
 * #179. Every committed edit and every drag frame used to run the same full pipeline (solve,
 * guideline graph, anchor links, plots, traffic), because URoadEditFacade::OnChanged carried
 * no way to say less had happened.
 *
 * GEOMETRY: a dragged node. Positions moved, but nothing was created, destroyed, split, or
 * reclassified, so the graph's SHAPE - which nodes exist, how they connect - is exactly what
 * it was. MoveNode and MoveApronCorner are the only sources of this kind.
 *
 * MARKINGS: SetIntermediateHoldingPosition, and (as of #179) nothing else. Neither the
 * pavement nor the graph's SHAPE changed - only a flag a paint layer reads did. This is
 * deliberately NOT folded into Geometry or Topology: Geometry's own rebuild
 * (RebuildSurfaceOnly) skips FHoldingPositionMarkingBuilder on purpose, because the graph it
 * would read is mid-drag and stale (see RebuildSurfaceOnly's comment) - wrong for a click that
 * needs the CURRENT graph repainted right away. Topology is wrong the other way: its
 * FRoadGuidelineBuilder::Build call reallocates every guideline node UNCONDITIONALLY, so
 * routing a holding-position toggle through it invalidated the very node whose flag had just
 * been set, and every other live FGuidelineNodeId in the level besides - three tests caught
 * this the day it was tried. Markings is the kind that is safe to repaint from the graph
 * exactly as it stands, because nothing about this edit could have made it stale.
 *
 * TOPOLOGY: everything else (PlaceNode, ConnectNodes, SplitSegment, DeleteNode,
 * DeleteSegment, AddApron/DeleteApron, PlaceEntity/DeleteEntity, runway facts, and the one
 * notify EndInteractiveEdit fires when a drag commits), because the derived graph -
 * guidelines, anchor links, stand layouts, plots, agent routes - can only be stale or wrong
 * if one of those changed.
 *
 * A PLAIN enum, not a UENUM: it travels on FOnNetworkChanged, an ordinary
 * DECLARE_MULTICAST_DELEGATE - never a UPROPERTY or a UFUNCTION parameter - so nothing here
 * is reflected and UHT never needs to see it (see CLAUDE.md on plain enums and UHT).
 */
enum class EChangeKind : uint8
{
	Geometry,
	Markings,
	Topology
};

/**
 * The facade a tool edits through, seen only as the calls a tool makes.
 *
 * Pattern: Facade (ARoadNetworkActor) exposed to Strategy (the IBuildTool family) through
 * an interface, so Tool/ has no COMPILE-TIME dependency on Present/. Before this seam, six
 * Tool/*.cpp files included Present/RoadNetworkActor.h purely for FToolContext::Target's
 * concrete type - and Present/ already includes Tool/ headers (RoadEditHistory, RoadHeal,
 * RoadSnap) for the actor's own facade methods, so a tool header including Present/ back
 * would have closed a real cycle. This header is that seam: it names exactly the calls
 * FToolContext::Target makes (enumerated with
 * `grep -ho 'Context\.Target->[A-Za-z_]*' Tool/*.cpp`), nothing more, and
 * ARoadNetworkActor implements it alongside being an AActor.
 *
 * A plain abstract class rather than a UINTERFACE: the tools are plain C++ structs, not
 * UObjects, and nothing in Blueprint needs to see this seam - a UInterface would add
 * reflection generation for a consumer that does not exist.
 */
class AIRSIDE_API IRoadEditTarget
{
public:
	virtual ~IRoadEditTarget() = default;

	/**
	 * The graph this target owns, or null before one exists.
	 *
	 * One read accessor rather than one per query, because every Tool/*.cpp reader only
	 * ever reads it - GetNodes, GetSegments, GetAprons, GetEntities, GetGuidelineNode and
	 * the rest are URoadNetwork's own const interface. Const so that stays true at the type
	 * level: a tool cannot reach a mutator through this pointer even by accident, and every
	 * mutation instead goes through a named method below that the facade can make undoable.
	 */
	virtual const URoadNetwork* GetNetwork() const = 0;

	/**
	 * Where the money for a build comes from, or null when building is free.
	 *
	 * ON THE TARGET so a TOOL can reach it through FToolContext::Target, the way it reaches
	 * everything else - the ghost needs to price what it is about to build and grey itself out
	 * when the player cannot pay. Default null rather than pure virtual: every existing
	 * implementer builds for nothing and should keep compiling.
	 */
	virtual IBuildPurse* GetPurse() const { return nullptr; }

	/**
	 * What connecting FromIndex to a point would cost, at the profile a click would actually
	 * lay. A free quote by default, which is what a target with no money answers.
	 *
	 * ON THE TARGET so the PREVIEW prices the same thing the click builds: a tool resolving
	 * the profile for itself would be a second answer to "which profile is this?", and the
	 * ghost would eventually quote one road while the click laid another.
	 */
	virtual FBuildQuote QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
		int32 WidthIndex) const { return FBuildQuote(); }

	/**
	 * The agents, read-only, for a tool that asks about them (Select). Model/, so Tool/ may
	 * see it; the Present-layer UAirsideTraffic stays invisible here.
	 *
	 * A DEFAULT rather than pure virtual: URoadEditFacade implements this interface too and
	 * genuinely has no traffic, and every other implementer forwards to the one that does.
	 */
	virtual const UGroundTraffic* GetGroundTraffic() const { return nullptr; }

	// --- Nodes and segments --------------------------------------------------------------

	virtual int32 PlaceNode(FVector2D Where) = 0;

	/**
	 * Runs a segment of Kind between two live nodes. See ERoadKind for why the KIND travels
	 * here and the profile does not.
	 *
	 * WidthIndex names a CHOICE, not an asset, which is what keeps that rule intact: the
	 * tool says "the third standard width" and the facade still resolves what that means
	 * and refuses a missing one, in the one place it already did. INDEX_NONE is "whatever
	 * this kind defaults to" - for a taxiway that is the actor's own instance tuning
	 * (ARoadNetworkActor::ResolveProfile), which is what every road laid before the width
	 * cycle existed used and must keep using.
	 */
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex) = 0;

	/** The kind's default width - what every caller before the width cycle meant. */
	bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind)
	{
		return ConnectNodes(FromIndex, ToIndex, Kind, INDEX_NONE);
	}

	/** A taxiway - what every caller before the fuel slice meant. A non-virtual overload,
	 *  so implementers override one signature; they carry `using IRoadEditTarget::ConnectNodes;`
	 *  so this one stays visible on the concrete type. */
	bool ConnectNodes(int32 FromIndex, int32 ToIndex)
	{
		return ConnectNodes(FromIndex, ToIndex, ERoadKind::Taxiway, INDEX_NONE);
	}

	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) = 0;
	/** Lays a runway with its surface and approach class written onto every segment of it. */
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts) = 0;

	/** The runway as the tool laid it before facts existed: tarmac, visual - the struct's defaults. */
	bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile)
	{
		return PlaceRunway(From, To, RunwayProfile, FRunwayFacts());
	}

	/**
	 * Reclassify the runway SegmentIndex belongs to - every segment of its chain - as one
	 * undoable edit. False for a dead slot or a segment that is not a runway.
	 */
	virtual bool SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts) = 0;

	/** MinimumRunwayLength, read-only: RunwayTool judges a drag against it but never sets it. */
	virtual double GetMinimumRunwayLength() const = 0;

	/**
	 * How many standard runway widths the content set declares.
	 *
	 * ADDED SO RunwayTool NO LONGER KNOWS UAirsideContent (issue #78): it used to call
	 * UAirsideSettings::GetContent() and load RunwayProfiles[Index] itself, the only file in
	 * Tool/ that did - every other tool goes through this seam for content, per CLAUDE.md's
	 * "Content resolves in Resolve* only". The tool keeps WidthIndex; this and
	 * ResolveRunwayProfile below are all it needs from the target.
	 */
	virtual int32 GetRunwayProfileCount() const = 0;

	/**
	 * The Nth standard runway profile, clamped to a live index by the implementer - see
	 * ARoadNetworkActor::ResolveRunwayProfile. Null when GetRunwayProfileCount() is zero.
	 */
	virtual URoadProfile* ResolveRunwayProfile(int32 Index) const = 0;

	/**
	 * How many standard taxiway widths the content set declares.
	 *
	 * THE RUNWAY PAIR ABOVE, FOR TAXIWAYS, and deliberately the same shape: the tool holds
	 * an index and knows nothing about UAirsideContent. Zero is a real answer - a project
	 * that has authored no taxiway profiles - and the tool says so rather than cycling
	 * through nothing in silence.
	 */
	virtual int32 GetTaxiwayProfileCount() const = 0;

	/**
	 * The Nth standard taxiway profile, clamped to a live index by the implementer. Null
	 * when GetTaxiwayProfileCount() is zero.
	 *
	 * SEPARATE FROM the default a taxiway gets with no index. That default is the actor's
	 * own Profile - per-instance tuning that ARoadNetworkActor::ResolveProfile keeps the
	 * content set out of on purpose - and this is the standard set a player cycles through.
	 * Two questions, two resolvers, and the level's tuning is not disturbed by the tool.
	 */
	virtual URoadProfile* ResolveTaxiwayProfile(int32 Index) const = 0;

	/**
	 * The cross-section a click with this Kind and WidthIndex would actually lay.
	 *
	 * THE ONE PLACE THIS RULE LIVES, as of 2026-09-20. It was written twice - in
	 * URoadEditFacade::ChooseProfile and in ARoadNetworkActor::UpdateGhost, whose comment
	 * already said the two must agree - and a third copy was about to be added for the guide
	 * anchor's half-width. Both existing sites now forward here, so the agreement is structural
	 * rather than maintained by hand.
	 *
	 * NOT A REPLACEMENT for ResolveTaxiwayProfile and its siblings: those answer "what is width
	 * 2", which is a content question. This answers "what would this GESTURE lay", which folds in
	 * the service road's exemption and the taxiway's fallback.
	 *
	 * NOT CONST, unlike its siblings above, and the reason is ARoadNetworkActor::ResolveProfile:
	 * a taxiway with no index falls back to the actor's own profile, which builds a RuntimeProfile
	 * from FallbackWidth the first time it is asked. A const signature here would be a promise
	 * this cannot keep.
	 */
	virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) = 0;

	virtual bool DisconnectGuideline(int32 EdgeIndex) = 0;

	/**
	 * Place (bSet) or clear an INTERMEDIATE holding position at a guideline node. Refuses a
	 * runway-holding position, which is derived and not the player's - spec 2026-09-07.
	 *
	 * INDICES, like every other call on this seam: a tool has picked a node out of
	 * GetNetwork()'s arrays and has no business constructing generation-checked handles -
	 * the facade makes them, and refuses a dead slot in one place.
	 */
	virtual bool SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet) = 0;
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) = 0;
	virtual bool DeleteNode(int32 NodeIndex) = 0;
	virtual bool DeleteSegment(int32 SegmentIndex) = 0;
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) = 0;

	/**
	 * Fold AbsorbIndex into KeepIndex - the merge a drop-on-node performs.
	 *
	 * KEEP IS THE NODE THE PLAYER AIMED AT and Absorb the one in their hand, so the thing
	 * they were pointing to is the thing that survives. A merge that kept the dragged node
	 * instead would move the target, which is the opposite of what the gesture says.
	 *
	 * REFUSES AND REVERTS rather than leaving two nodes at one position: see
	 * URoadEditFacade::MergeNodes, and URoadEditHistory::RevertEdit on why refusing after
	 * the fact needs undoing rather than abandoning.
	 */
	virtual bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex) = 0;
	virtual void BeginInteractiveEdit(const FString& Label) = 0;
	virtual void EndInteractiveEdit(bool bKeep) = 0;
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const = 0;

	// --- Aprons ----------------------------------------------------------------------------

	virtual int32 AddApron(const TArray<FVector2D>& Outline) = 0;
	virtual bool DeleteApron(int32 ApronIndex) = 0;
	virtual int32 FindApronAt(FVector2D Where) const = 0;

	/**
	 * Move one corner of an apron outline - the Edit mode's apron handle.
	 *
	 * REFUSES A MOVE THAT CROSSES THE OUTLINE, because a self-intersecting polygon has no
	 * inside and the surface builder has no answer for one. Judged through
	 * RoadGeom::IsSimplePolygon, the same test FApronDrawTool already closes an outline
	 * against, rather than a second opinion about what a valid apron is.
	 */
	virtual bool MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To) = 0;

	// --- Entities ------------------------------------------------------------------------

	/** Drops one installation of Kind at a pose. See EPlaceableEntity for why the KIND
	 *  travels here and the definition does not. */
	virtual int32 PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) = 0;

	/** A stand - what every caller before the fuel slice meant. A non-virtual overload, so
	 *  implementers override one signature; they carry `using IRoadEditTarget::PlaceStand;`
	 *  where the name would otherwise be hidden. */
	int32 PlaceStand(FVector2D Where, double Heading)
	{
		return PlaceEntity(Where, Heading, EPlaceableEntity::Stand);
	}

	/**
	 * Drop an installation into a DRAWN plot, filling its bays with Modules.
	 *
	 * NOT AN OVERLOAD OF PlaceEntity, because the pose is not the caller's to give: it comes
	 * from the fit against whichever plot edge faces a road, and a tool that passed a pose
	 * here would be stating an answer the solver owns.
	 *
	 * INDEX_NONE when the plot has no road frontage or is smaller than one bay. The facade
	 * logs which; see URoadEditFacade::PlaceEntityInPlot.
	 */
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>& Outline,
		FVector2D FrontageA, FVector2D FrontageB,
		const TArray<EDepotModule>& Modules, EPlaceableEntity Kind) = 0;

	virtual bool DeleteEntity(int32 EntityIndex) = 0;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const = 0;

	/** The definition of Kind, read-only: a tool previews what would be placed, never
	 *  authors it. RESOLVED, the same object PlaceEntity places from - see
	 *  ARoadNetworkActor::ResolveEntityDefinition. */
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity Kind) const = 0;

	/** The stand's, for every caller written before there was a second kind. */
	const UEntityDefinition* GetStandDefinition() const
	{
		return GetEntityDefinition(EPlaceableEntity::Stand);
	}

	// --- Ghost preview -------------------------------------------------------------------

	/** WidthIndex as ConnectNodes: a choice, INDEX_NONE for the kind's default. It is here
	 *  for the reason the overload below already gives - a ghost that previewed the default
	 *  while the click laid a cycled width would be the same lie in a new place. */
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
		ERoadKind Kind, int32 WidthIndex) = 0;

	/** The kind's default width. */
	void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid, ERoadKind Kind)
	{
		UpdateGhost(FromNodeIndex, Snap, bValid, Kind, INDEX_NONE);
	}

	/** A taxiway, as ConnectNodes. The ghost must show the width the click will ACTUALLY
	 *  lay: a 23 m preview over a 6 m road is a lie the player then acts on. */
	void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid)
	{
		UpdateGhost(FromNodeIndex, Snap, bValid, ERoadKind::Taxiway, INDEX_NONE);
	}

	virtual void HideGhost() = 0;
	virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const = 0;

	// --- Routing and agents ------------------------------------------------------------

	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan) const = 0;

	/**
	 * One struct, not four - see FAirframe. Ground, Climb and Engine used to be separate
	 * parameters, which is how issue #27 happened: a caller could pass one and default
	 * another, so the taxi and a later handover were never guaranteed to read the SAME
	 * aeroplane. Issue #30 finished the collapse begun there.
	 */
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe,
		ETraversalClass Class) = 0;

	/** Aircraft by default - what every caller before M2 meant. A non-virtual overload,
	 *  so implementers override one signature; they carry `using IRoadEditTarget::DispatchAgent;`
	 *  so this one stays visible on the concrete type. */
	bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe)
	{
		return DispatchAgent(Plan, Airframe, ETraversalClass::Aircraft);
	}

	virtual void RebuildMesh() = 0;
};
