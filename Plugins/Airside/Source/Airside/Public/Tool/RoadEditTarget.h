#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Entities/EntityDefinition.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"
#include "Solve/PlotYard.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadSnap.h"

class URoadNetwork;
class IBuildPurse;
class URoadProfile;
class UGroundTraffic;
class UEntityDefinition;
struct FAirframe; // #300: DispatchAgent takes it by reference and forwards it on; RoadEntity.h no longer pulls in Airframe.h, so this header names its own forward declaration.

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
 * DeleteSegment, AddApron/DeleteApron, PlaceEntity/DeleteEntity, the drive side, and the one
 * notify EndInteractiveEdit fires when a drag commits), because the derived graph -
 * guidelines, anchor links, stand layouts, plots, agent routes - can only be stale or wrong
 * if one of those changed.
 *
 * FACTS (#446): a FACT the derivation does not read changed - a runway's facts (SetRunwayFacts),
 * a depot's modules (AddEntityModule, RemoveUnseatedModules). The road's shape and the guideline
 * graph are exactly what they were, so NOTHING IS RE-DERIVED and no handle is reallocated: the
 * surface is re-meshed (a runway's pavement is a mesh slot, its facts its paint) and the actor's
 * OnNetworkChanged tells the buildings (a bought shed lights a bay) and ops. Until #446 these were
 * Topology, so a shed purchase re-derived the whole airport and re-pointed every agent's route,
 * and the caches that read the facts saw them change only because that re-derivation moved
 * GuidelineRevision. The model moves that clock for a fact itself now (URoadNetwork::
 * NoteFactChanged), which is what makes this kind SAFE: optimising an edit down to Facts cannot
 * silently stale a cache that reads what it wrote.
 * NOT AirsideDerivation::EDeriveScope::Facts, which shares the word and nothing else: that scope is the taxiway
 * restriction's quiet what-if on a copy; this kind derives through EDeriveScope::Surface (RebuildFactsOnly).
 * ENFORCED BY: AirportOps.Model.Offers.Generate.RunwayFlipAsksAgain, AirportOps.Service.Rebid.RunwayFlipRebids,
 * Airside.Present.Facility.ModulePurchaseRelightsWithoutRederiving
 *
 * A PLAIN enum, not a UENUM: it travels on FOnNetworkChanged, an ordinary
 * DECLARE_MULTICAST_DELEGATE - never a UPROPERTY or a UFUNCTION parameter - so nothing here
 * is reflected and UHT never needs to see it (see CLAUDE.md on plain enums and UHT).
 * A SWITCH ON IT IS TOTAL (ChangeKindName, the actor's rebuild, every OnNetworkChanged listener):
 * wrapped in AIRSIDE_EXHAUSTIVE_SWITCH_*, so a fifth kind is a build error at each, not a quiet
 * fall into whichever branch an if-chain reached last.
 */
enum class EChangeKind : uint8
{
	Geometry,
	Markings,
	Topology,
	Facts
};

/** The kind's name, for a log line. */
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
inline const TCHAR* ChangeKindName(EChangeKind Kind)
{
	switch (Kind)
	{
	case EChangeKind::Geometry: return TEXT("Geometry");
	case EChangeKind::Markings: return TEXT("Markings");
	case EChangeKind::Topology: return TEXT("Topology");
	case EChangeKind::Facts:    return TEXT("Facts");
	}
	return TEXT("?");
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

/**
 * The one rebuild that covers two notifies - what a REBUILD BATCH (FRoadRebuildBatch, below)
 * records in place of each notify it folds.
 *
 * NOT std::max ON THE ENUM ORDER, although that is what it reads like for every pair but one:
 * Geometry and Markings are DISJOINT, not ranked. Geometry's rebuild (RebuildSurfaceOnly)
 * skips the holding paint; Markings' (RebuildMarkingsOnly) skips the surface - so a batch that
 * saw one of each owes BOTH, and the only single rebuild that does both is Topology (which
 * repaints the surface, and re-derives the graph and re-applies holding marks before painting
 * them - FRoadGuidelineBuilder's ReapplyHoldingPositionMarks). Any pair naming Topology is
 * Topology, because Topology is a superset of the other three. Same kind twice is that kind.
 * FACTS WITH ANYTHING ELSE IS TOPOLOGY TOO (#446), on purpose rather than by the table's default:
 * Facts re-meshes but does not repaint the holding bars (Markings owes that), and its listeners
 * read the guideline graph as it stands - which a Geometry notify in the same batch has left behind
 * the road. Only Topology covers either pairing, and a batch is a bulk edit, never a drag frame.
 *
 * World-free and inline so Airside.Present.RebuildBatch can pin the table on its own, below
 * the composition it also tests.
 */
inline EChangeKind CombineChangeKinds(EChangeKind A, EChangeKind B)
{
	return A == B ? A : EChangeKind::Topology;
}

/**
 * Where a WHOLESALE REPLACEMENT of the live network is, for URoadEditFacade::OnReplaced (#426). A replacement is the
 * one change a tool's part-drawn state cannot survive: a tool remembers SLOT INDICES, and a replacement it did not ask
 * for gives those indices to different things. Undo, Redo, ClearNetwork and a save-game load (RestoreInPlace) are one.
 *
 * DISCARDING: the live network is about to be thrown away for one that shares NO HISTORY with it - ClearNetwork and a
 * load, never Undo/Redo. Fired while the old graph is still live, because that is the last moment a tool can abandon
 * against the graph its indices name: FRoadChainingState::OnCancel deletes the bare node its chain made, BY INDEX, and
 * run after a load it would delete whatever bare node the loaded airport keeps in that slot.
 *
 * ADOPTED: the replacement is live and rebuilt - fired by all four, after OnChanged. Undo and Redo fire ONLY this: the
 * history has already moved when Travel runs, so an abandon before it would commit an edit into the middle of the
 * travel (a new undo step, and the redo stack cleared under the step being redone); and a Memento keeps its graph's
 * slots, so a tool abandoning after an undo acts on the slots it knew.
 *
 * A PHASE, SO ONE ENUM (CLAUDE.md): a listener binds once and switches, rather than pairing two delegates by hand.
 * Plain, not a UENUM, for EChangeKind's reason above: it travels on a native delegate only.
 */
enum class ENetworkReplace : uint8
{
	Discarding,
	Adopted
};

/**
 * The facade a tool edits through, seen only as the calls a tool makes.
 *
 * Pattern: Facade (ARoadNetworkActor) exposed to Strategy (the IBuildTool family) through
 * an interface, so Tool/ has no COMPILE-TIME dependency on Present/. Before this seam, six
 * Tool/*.cpp files included Present/RoadNetworkActor.h purely for FToolContext::Target's
 * concrete type - and Present/ already includes Tool/ headers (RoadHeal, RoadSnap) for the
 * actor's own facade methods (RoadEditHistory too, until issue #191 moved it to Present/
 * alongside the facade it serves), so a tool header including Present/ back would have
 * closed a real cycle. This header is that seam: it names exactly the calls
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
	 *
	 * Surface as ConnectNodes takes it - grass is priced below tarmac (BuildCost::ForSegment),
	 * so a quote that ignored it would promise the player one price and charge another.
	 */
	virtual FBuildQuote QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
		int32 WidthIndex, EPavement Surface) const { return FBuildQuote(); }

	/**
	 * What a runway from From to To at Profile, surfaced with Pavement, would cost -
	 * QuoteForConnect's reason, for runways: PlaceRunway prices through this same function, so
	 * the ghost cannot quote one strip while the click charges another. Free by default, as a
	 * target with no money is.
	 *
	 * Pavement AS THE TOOL'S CHOSEN SURFACE, not the profile's - a runway's ground is
	 * FRunwayFacts', not the profile's, and the tool names it (FRunwayTool::Surface) before the
	 * strip exists to read it back from.
	 */
	virtual FBuildQuote QuoteForRunway(FVector2D From, FVector2D To, const URoadProfile* Profile,
		EPavement Pavement) const
	{
		return FBuildQuote();
	}

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
	 *
	 * Surface is what the new segment is laid on (FRoadSegment::Surface), written inside the
	 * same undoable edit as the segment - a separate SetSurface call after this would be two
	 * undo steps for one click, and a road that briefly existed as tarmac to every rebuild.
	 */
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex,
		EPavement Surface) = 0;

	/**
	 * Why a road or taxiway from FromIndex to the snapped point To, at this Kind and width,
	 * may not be laid - inside a taxiway's clearance strip it does not meet square, or (a
	 * taxiway) with its own strip over something built. Empty = allowed. The strip judge only;
	 * RoadPlacement::Validate still owns the geometric rules and ConnectNodes the price.
	 *
	 * THE ONE EVALUATOR, WhyStandRefused's pattern for roads (strip stage 3): the draw tool's
	 * readout, its ghost, its click and ConnectNodes all ask this, so the preview can never
	 * approve what the commit refuses. A SNAP, not a node index, for To: a Segment snap names
	 * the ORIGINAL segment before the click splits it, which is what lets the preview (unsplit)
	 * and the commit (split, a Node) give one answer.
	 * ENFORCED BY: Airside.Tool.RoadRefusedInsideStrip
	 */
	virtual FString WhySegmentRefused(int32 FromIndex, const FRoadSnapResult& To, ERoadKind Kind, int32 WidthIndex) const = 0;

	/** Tarmac - what every caller before the surface row meant. */
	bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex)
	{
		return ConnectNodes(FromIndex, ToIndex, Kind, WidthIndex, EPavement::Tarmac);
	}

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

	/**
	 * Lets vehicles back from the arm NodeIndex->FromFarIndex through NodeIndex into the arm
	 * NodeIndex->IntoFarIndex (FReverseTurn, spec 2026-09-26 §3). Node indices, as ConnectNodes
	 * takes. False, and nothing recorded, when a node is dead or either pair is not a road arm of
	 * NodeIndex; whether the reverse leg can actually be LAID is the rebuild's, and it logs why not.
	 */
	virtual bool AddReverseTurn(int32 NodeIndex, int32 FromFarIndex, int32 IntoFarIndex) = 0;
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

	/**
	 * Upgrade mode's commit (strip stage 6): give an existing road or taxiway the standard width
	 * WidthIndex of Kind and the Surface, IN PLACE, as ONE undoable edit - so one Ctrl+Z reverts
	 * both. Priced as the new ground less the old, never a refund on a downgrade. True with no
	 * edit when it already is so (SetRunwayFacts' rule); false, and nothing touched, whenever
	 * WhyUpgradeRefused has a reason.
	 *
	 * KIND TRAVELS, beside the plan's (SegmentIndex, WidthIndex, Surface): a width index names a
	 * choice WITHIN a kind (ConnectNodes' rule), so the tool says which list it picked from and a
	 * Road tool's tier clicked on a taxiway is refused rather than silently re-read as a code.
	 */
	virtual bool UpgradeSegment(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) = 0;

	/**
	 * Why UpgradeSegment would refuse, empty = allowed. THE ONE EVALUATOR - the Upgrade mode's
	 * hover label, its ghost colour and the click all ask it (WhySegmentRefused's pattern): dead
	 * slot, a runway or a kind change, no such width, an unoffered surface, the price, and the
	 * widened PAVEMENT inside another taxiway's strip. What the grown STRIP swallows is NOT a
	 * refusal - that restricts the taxiway or closes a stand (TaxiwayRestriction, stage 6).
	 * ENFORCED BY: Airside.Present.UpgradeSegment, Airside.Present.UpgradeSegmentRefusesIntoNeighbourStrip
	 */
	virtual FString WhyUpgradeRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const = 0;

	/**
	 * The SITE half of WhyUpgradeRefused (issue #439's sibling of WhyStandSiteRefused): every gate
	 * that reads the model and the picks - a dead slot, a runway or kind change, no such width, an
	 * unoffered surface, the widened pavement inside a neighbour's strip - and none that reads
	 * money. THE HALF A CALLER MAY MEMOISE, against GetEditEpoch and its own picks.
	 *
	 * SITE FIRST, then WhyUpgradeUnaffordable, is the composition WhyUpgradeRefused now is. That
	 * moved the price gate AFTER the strip gate, where it used to sit before it: an upgrade both
	 * unaffordable and refused by a neighbour's strip now says the strip, because earning the
	 * money cannot make it committable and "cannot afford" would promise that it could.
	 * ENFORCED BY: Airside.Present.UpgradeRefusalIsTheTwoHalves
	 */
	virtual FString WhyUpgradeSiteRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const = 0;

	/**
	 * The MONEY half of WhyUpgradeRefused: "cannot afford ..." when the purse cannot pay the
	 * difference between the new ground and the old, else empty. Asked FRESH by every caller and
	 * never memoised - the balance moves through no edit of the model (see WhyStandUnaffordable). EMPTY when there is nothing to price: a dead slot, a width or
	 * kind that does not resolve (the site half's to refuse), or an upgrade that changes nothing.
	 */
	virtual FString WhyUpgradeUnaffordable(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const = 0;

	/** MinimumRunwayLength, read-only: RunwayTool judges a drag against it but never sets it. */
	virtual double GetMinimumRunwayLength() const = 0;

	/**
	 * How many standard runway widths the content set declares.
	 *
	 * ADDED SO RunwayTool NO LONGER KNOWS UAirsideContent (issue #78): it used to call
	 * UAirsideSettings::GetContent() and load RunwayProfiles[Index] itself, the only file in
	 * Tool/ that did - every other tool goes through this seam for content, per CLAUDE.md's
	 * "Content resolves in Resolve* only".
	 * ENFORCED BY: Check-Architecture.ps1 rule 1's Content clause (Tool/ may not include
	 * Content/). The tool keeps WidthIndex; this and ResolveRunwayProfile below are all it
	 * needs from the target.
	 *
	 * THE CLAIM ABOVE WENT FALSE ONCE ALREADY: FPlotPlaceTool grew its own include of the
	 * content settings header for the depot kit table (issue #181) before ResolveDepotKits
	 * below closed the same gap a second time. Prose did not hold the line the first time, so
	 * Check-Architecture.ps1's rule 1 now forbids the Content/ folder from Tool/ mechanically,
	 * the same way it already forbade Present/ - a third lapse fails the build instead of
	 * waiting for a review.
	 */
	virtual int32 GetRunwayProfileCount() const = 0;

	/**
	 * The Nth standard runway profile, clamped to a live index by the implementer - see
	 * ARoadNetworkActor::ResolveRunwayProfile. Null when GetRunwayProfileCount() is zero.
	 */
	virtual URoadProfile* ResolveRunwayProfile(int32 Index) const = 0;

	/**
	 * How many standard widths the content set declares for Kind: the ICAO taxiway codes,
	 * or the Narrow / Standard / Wide road tiers.
	 *
	 * ONE PAIR KEYED BY KIND since 2026-09-23. It was GetTaxiwayProfileCount /
	 * ResolveTaxiwayProfile, taxiway-only, and road tiers would have been a second parallel
	 * pair - two lists that must agree about "what can this tool cycle", the failure CLAUDE.md
	 * names. The tool holds an index and knows nothing about UAirsideContent. Zero is a real
	 * answer - a project that has authored none - and the tool says so rather than cycling
	 * through nothing in silence.
	 */
	virtual int32 GetWidthCount(ERoadKind Kind) const = 0;

	/**
	 * The Nth standard profile for Kind, clamped to a live index by the implementer. Null
	 * when GetWidthCount(Kind) is zero.
	 *
	 * SEPARATE FROM the default a road gets with no index. For a taxiway that default is the
	 * actor's own Profile - per-instance tuning ARoadNetworkActor::ResolveProfile keeps the
	 * content set out of on purpose - and for a service road it is the actor's override or
	 * the narrowest tier. This is the standard set a player cycles through.
	 */
	virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const = 0;

	/**
	 * The cross-section a click with this Kind and WidthIndex would actually lay.
	 *
	 * THE ONE PLACE THIS RULE LIVES, as of 2026-09-20. It was written twice - in
	 * URoadEditFacade::ChooseProfile and in ARoadNetworkActor::UpdateGhost, whose comment
	 * already said the two must agree - and a third copy was about to be added for the guide
	 * anchor's half-width. Both existing sites now forward here, so the agreement is structural
	 * rather than maintained by hand.
	 *
	 * NOT A REPLACEMENT for ResolveWidthProfile and its siblings: those answer "what is width
	 * 2", which is a content question. This answers "what would this GESTURE lay", which folds in
	 * each kind's default: the service road's override or narrowest tier, the taxiway's fallback.
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
	 * URoadEditFacade::MergeNodes, and URoadEditHistory::RollbackEdit on why refusing after
	 * the fact needs undoing rather than abandoning.
	 */
	virtual bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex) = 0;
	virtual void BeginInteractiveEdit(const FString& Label) = 0;
	virtual void EndInteractiveEdit(bool bKeep) = 0;

	/**
	 * Open / close a REBUILD BATCH: every change notify between the two is folded into ONE
	 * derived rebuild when the OUTERMOST batch closes. Counted, so batches nest. Call through
	 * FRoadRebuildBatch below, never by hand - a Begin whose End an early return skipped
	 * would defer every rebuild in the level forever. See URoadEditFacade's class comment for
	 * the semantics, and how a batch meets drags, undo, MergeNodes and RollBackOpenEdit.
	 *
	 * PURE, not a no-op default the way GetPurse is: a target that silently ignored a batch
	 * would still be CORRECT (it just rebuilds N times), which is exactly why nobody would
	 * notice it forgot - the compiler asks every implementer to decide instead.
	 */
	virtual void BeginRebuildBatch() = 0;
	virtual void EndRebuildBatch() = 0;

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

	/**
	 * Turn an ACCEPTED drawn rectangle into a stand - see WhyStandRefused for the refusal
	 * this is expected to have already cleared. EntranceA/EntranceB are the same two points
	 * as Outline's own entrance edge (Outline[0]/[1] in the convention StandBox.h
	 * documents), carried separately because a caller correcting Outline's winding must
	 * swap them together - see URoadEditFacade::PlaceStandInPlot, which mirrors
	 * PlaceEntityInPlot's own CCW correction for exactly that reason.
	 *
	 * NOT AN OVERLOAD OF PlaceEntity, for the same reason PlaceEntityInPlot is not: the pose
	 * is derived from the drawn box and the letter it reads as, not stated by the caller.
	 *
	 * Returns the entity index, or INDEX_NONE - WhyStandRefused names why, and the facade
	 * logs it.
	 *
	 * Pavement is the pad's (the stand tool's Surface row), captured onto the instance and
	 * priced into the charge. NO DEFAULT ARGUMENT: a default on a virtual binds by the
	 * caller's static type, and a forgotten caller would place tarmac without a word.
	 */
	virtual int32 PlaceStandInPlot(const TArray<FVector2D>& Outline,
		FVector2D EntranceA, FVector2D EntranceB, EPavement Pavement) = 0;

	/**
	 * Why a drawn rectangle cannot become a stand - empty means it can.
	 *
	 * THE ONE EVALUATOR, the #182 lesson applied to stands: a tool's readout and
	 * PlaceStandInPlot's commit both ask this rather than keeping their own opinions, so a
	 * preview can never approve what the commit refuses, or the reverse. See
	 * URoadEditFacade::WhyStandRefused for the refusal order (self-crossing, too small
	 * against the smallest stand letter's floor, an unfit letter, an overlap, a taxiway through the
	 * interior, then afford) and why each check is winding-independent, so this may be
	 * asked of Outline exactly as drawn, before any CCW correction.
	 *
	 * Pavement is the pad the commit would lay - the afford gate prices it, so the readout
	 * and the Build click agree on what a grass stand costs. No default, for
	 * PlaceStandInPlot's reason.
	 *
	 * IN TWO HALVES since issue #439, which this is the composition of: WhyStandSiteRefused,
	 * then WhyStandUnaffordable. A tool that wants to remember the answer may remember only the
	 * first - see each half's own comment for why the second must not be remembered.
	 * ENFORCED BY: Airside.Present.StandPlot.WhyStandRefusedIsTheTwoHalves
	 */
	virtual FString WhyStandRefused(TArrayView<const FVector2D> Outline, EPavement Pavement) const = 0;

	/**
	 * The SITE half of WhyStandRefused: everything that reads the outline and the model and
	 * nothing that reads money - self-crossing, too small, an unfit letter, an overlap, a taxiway
	 * through the interior, a clearance strip. Empty means the ground would take a stand.
	 *
	 * THE HALF A CALLER MAY MEMOISE, against GetEditEpoch: it is a function of the outline and
	 * the model, and the epoch moves on every edit of the model that notifies (see its exception).
	 * No Pavement parameter, because
	 * no gate in it reads one - every pavement passes or fails the site alike.
	 */
	virtual FString WhyStandSiteRefused(TArrayView<const FVector2D> Outline) const = 0;

	/**
	 * The MONEY half of WhyStandRefused: "cannot afford ..." when the purse cannot pay for a stand
	 * on this Outline in this Pavement, else empty. The ghost already asks CanAfford every frame
	 * (IBuildPurse::CanAfford's own doc) - and this MUST be asked fresh, never memoised: the balance moves on its own
	 * (landing fees, other purchases, refunds) through no edit of the model, so GetEditEpoch cannot
	 * see it. Issue #439 was a memo that could not.
	 *
	 * EMPTY WHEN THERE IS NOTHING TO PRICE - no letter, or no template for it. The site half
	 * refuses both, so that refusal is its to say, and the composition asks it first. (An outline
	 * the site refuses for another reason may still be priced here, and refused; asked alone,
	 * this half says only whether the money is there.)
	 */
	virtual FString WhyStandUnaffordable(TArrayView<const FVector2D> Outline, EPavement Pavement) const = 0;

	/**
	 * A counter that moves on every edit of the model that goes through the facade - node, segment,
	 * apron, entity, undo, redo, a network swapped in or cleared - and on nothing else. What a memo
	 * of an answer that reads the model keys on, beside the outline it was asked about.
	 *
	 * EXCEPT A SAVE-GAME LOAD, per issue #426's trace (open, and no test here pins it):
	 * the ops runtime's LoadFromSlot deserialises into the live network in place with no NotifyChanged,
	 * so the epoch does not move (and the network pointer is the same, so no second key would catch
	 * it either). A memo held across a load can be stale until the next edit or gesture boundary;
	 * the load's own protocol is #426's to write, not this counter's to paper over.
	 *
	 * WHY NOT URoadNetwork::GetEditRevision, which is the counter a memo would reach for: it is
	 * scoped to nodes and segments, so PlaceEntity and RemoveEntity - a stand placed into the
	 * outline - do not move it, and it is not a UPROPERTY, so an undone-to network starts its own
	 * clock again from wherever it was duplicated. This one is counted at the facade's one
	 * notification door (URoadEditFacade::NotifyChanged), which an edit passes through to be
	 * rebuilt at all - so a new mutator gets it by notifying, not by remembering a second call.
	 * ENFORCED BY: Airside.Present.EditEpoch.MovesOnEveryEditDoor (a place, a delete, an undo and a clear)
	 */
	virtual uint32 GetEditEpoch() const = 0;

	/**
	 * Why a depot plot with this Outline may not be placed, or empty. WhyStandRefused's
	 * pattern for plots (strip stage 3): FPlotPlaceTool's readout and Build button and
	 * PlaceEntityInPlot's commit all ask this, so the bar cannot light Build over a plot the
	 * commit refuses. The outline alone - self-crossing, over a stand, inside a taxiway's
	 * clearance strip - in that order; what only the commit knows (no definition, a
	 * reservation that fits nothing, afford) stays the commit's.
	 * ENFORCED BY: Airside.Tool.PlotPlace.RefusedInsideStrip
	 */
	virtual FString WhyPlotRefused(TArrayView<const FVector2D> Outline) const = 0;

	virtual bool DeleteEntity(int32 EntityIndex) = 0;

	/**
	 * The entity a click at Where means: a stand within Radius of its stop mark, else a plot
	 * whose outline contains Where - its ground and every building on it. INDEX_NONE for
	 * neither. A plot is never picked by its gate; see URoadEditFacade::FindEntityAt.
	 */
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

	/**
	 * Every module kind a depot's plot can hold, as specs the yard solver understands.
	 *
	 * ADDED SO FPlotPlaceTool NO LONGER KNOWS UAirsideContent (issue #181) - THE SAME SEAM
	 * GetRunwayProfileCount ABOVE CUT FOR THE RUNWAY TOOL AT #78, for a tool that had grown
	 * the identical dependency a second time. UPlotPresenter resolves the SAME table through
	 * ARoadNetworkActor::ResolveDepotKits (see URoadEditFacade's forwarder) rather than
	 * calling DepotKitSpecs a second time, so the ghost a tool draws and the depot the
	 * presenter builds read one source rather than two that happen to agree.
	 */
	virtual TArray<PlotYard::FKitSpec> ResolveDepotKits() const = 0;

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

	/** Errand names the routing policy - runway avoidance, penalty, whether the occupancy
	 *  table is read. See FRoutePolicy. Defaulted, because every caller of this seam today
	 *  is a player or a tool asking for a route directly. */
	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan,
		ERouteErrand Errand = ERouteErrand::PlayerIssued) const = 0;

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

	/**
	 * The same, for a service vehicle - an FVehicle, not an FAirframe with its climb zeroed
	 * (2026-09-23). AN OVERLOAD BY BUNDLE, so the argument says what kind of thing is sent and
	 * FRoadAgent's body follows from it; Class stays the routing fact it always was.
	 */
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FVehicle& Vehicle,
		ETraversalClass Class) = 0;

	/** GroundVehicle by default, the vehicle counterpart of the Aircraft overload above. */
	bool DispatchAgent(const FRoutePlan& Plan, const FVehicle& Vehicle)
	{
		return DispatchAgent(Plan, Vehicle, ETraversalClass::GroundVehicle);
	}

	virtual void RebuildMesh() = 0;
};

/**
 * N mutations, ONE derived rebuild: opens a rebuild batch on Target for this scope's lifetime
 * (IRoadEditTarget::BeginRebuildBatch/EndRebuildBatch). For a bulk edit laid in one synchronous
 * call - FRigCourseLayout::Lay placed ~35 segments one ConnectNodes at a time, each running the
 * whole solve + guideline + anchor + plots + traffic pipeline, ~1.7 s a course (2026-09-27).
 *
 * Pattern: RAII scope guard (the FScopeLock shape), so an early return still closes the batch.
 * A STACK LOCAL, NAMED: `FRoadRebuildBatch(Target);` is a temporary that closes on the same
 * line, batching nothing - [[nodiscard]] warns on it and Check-Architecture's rule 25 fails
 * it, along with any batch held by pointer or as a member (a batch that outlives one call
 * would span frames, and every frame in between would draw a stale graph).
 *
 * DERIVED STATE IS STALE WHILE IT IS OPEN, BY DESIGN: the guideline graph, anchor links,
 * plots, traffic routes and meshes still describe the network as it was when the batch
 * opened. Read the MODEL (nodes, segments, reverse turns) freely; read nothing derived until
 * the guard is gone. See URoadEditFacade's class comment for the mutators that read derived
 * state themselves, and warn if called in a batch that has deferred a rebuild.
 */
class FRoadRebuildBatch
{
public:
	[[nodiscard]] explicit FRoadRebuildBatch(IRoadEditTarget& InTarget) : Target(InTarget)
	{
		Target.BeginRebuildBatch();
	}

	~FRoadRebuildBatch()
	{
		Target.EndRebuildBatch();
	}

	FRoadRebuildBatch(const FRoadRebuildBatch&) = delete;
	FRoadRebuildBatch& operator=(const FRoadRebuildBatch&) = delete;

private:
	IRoadEditTarget& Target;
};
