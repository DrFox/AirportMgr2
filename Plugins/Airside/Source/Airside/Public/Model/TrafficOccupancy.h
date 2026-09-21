#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "TrafficOccupancy.generated.h"

/** What kind of thing an agent can hold. See FTrafficResource. */
UENUM()
enum class ETrafficResourceKind : uint8
{
	/** A guideline edge, held as a distance INTERVAL along it - a queue, not a block. */
	Edge,
	/** A guideline node. Exclusive: junctions and crossings are the conflict points. */
	Node,
	/** One segment of a runway. A landing holds every segment of the chain; a holding-position
	 *  node names one and the arbiter expands it. */
	Surface,
};

/**
 * One thing an agent can hold: an edge, a node, or a runway segment.
 *
 * A tagged struct rather than three tables, so the runway grant M3's sequencer will ask
 * for (spec 3.8: "runway occupancy is one more surface in the same table") is the same
 * TryClaim a junction uses - one rule for a taxiway crossing a runway and for a landing.
 * TVariant was rejected: it is not UHT-reflectable, and every claim has to be a UPROPERTY
 * so the table can live on a UObject the collector traces.
 */
USTRUCT()
struct AIRSIDE_API FTrafficResource
{
	GENERATED_BODY()

	UPROPERTY() ETrafficResourceKind Kind = ETrafficResourceKind::Node;
	UPROPERTY() FGuidelineEdgeId Edge;
	UPROPERTY() FGuidelineNodeId Node;
	UPROPERTY() FRoadSegmentId Surface;

	static FTrafficResource OfEdge(FGuidelineEdgeId Id);
	static FTrafficResource OfNode(FGuidelineNodeId Id);
	static FTrafficResource OfSurface(FRoadSegmentId Id);

	bool operator==(const FTrafficResource& Other) const;
	bool operator!=(const FTrafficResource& Other) const { return !(*this == Other); }

	/** "edge 12", "node 4", "runway segment 2" - for the log lines. */
	FString Describe() const;
};

/**
 * Hashes exactly what operator== compares - Kind plus the one handle that Kind says is
 * live - so two resources the operator calls equal always land in the same TMap bucket.
 * Needed for FTrafficOccupancy's per-resource index (issue #168): without it, TMap<
 * FTrafficResource, ...> does not compile.
 */
FORCEINLINE uint32 GetTypeHash(const FTrafficResource& Resource)
{
	switch (Resource.Kind)
	{
	case ETrafficResourceKind::Edge:    return HashCombine(GetTypeHash(Resource.Kind), GetTypeHash(Resource.Edge));
	case ETrafficResourceKind::Node:    return HashCombine(GetTypeHash(Resource.Kind), GetTypeHash(Resource.Node));
	case ETrafficResourceKind::Surface: return HashCombine(GetTypeHash(Resource.Kind), GetTypeHash(Resource.Surface));
	default:                            return GetTypeHash(Resource.Kind);
	}
}

/**
 * One agent's hold on one resource.
 *
 * OCCUPIED versus RESERVED is the whole arbitration rule (spec 3.3). A claim that contains
 * the agent's own position is occupied and can never be taken away - nobody is evicted from
 * a node they are standing in. Anything ahead is a reservation, and a higher rank preempts
 * it. Rank is decided by the caller (class priority, or the node's PriorityOverride) so the
 * table never has to know what a class is.
 */
USTRUCT()
struct AIRSIDE_API FTrafficClaim
{
	GENERATED_BODY()

	UPROPERTY() int32 AgentId = 0;
	UPROPERTY() FTrafficResource Resource;

	/** Edge kind only: the interval held, in edge distance from A. Half-open [From, To). */
	UPROPERTY() double From = 0.0;
	UPROPERTY() double To = 0.0;

	UPROPERTY() bool bOccupied = false;
	UPROPERTY() int32 Rank = 0;

	/**
	 * True when the two cannot both stand. Half-open on edges, so a queue that packs
	 * intervals end to end is not told it is colliding with itself.
	 */
	bool Conflicts(const FTrafficClaim& Other) const;

	/**
	 * A node or whole-surface claim in one expression, rather than default-constructing and
	 * setting AgentId/Resource/bOccupied/Rank by hand - six call sites did (#103). Leaves
	 * From/To at their edge-claim default (0.0): every one of those six is a node or surface
	 * claim, never an edge interval.
	 */
	static FTrafficClaim Make(int32 AgentId, const FTrafficResource& Resource, bool bOccupied, int32 Rank = 0);
};

UENUM()
enum class EClaimResult : uint8
{
	Granted,
	/** Refused; the blocking claim is handed back so the caller can stop short of it. */
	Held,
};

/**
 * Who holds which guideline edge interval, node and runway surface, and what each has
 * reserved a short way ahead. Spec 3.8's FTrafficOccupancy; spec 2026-09-06 §2.1.
 *
 * MECHANISM, NOT POLICY. It grants and refuses by the one rule in TryClaim and knows
 * nothing about braking distances, classes or routes - UGroundTraffic decides what to ask
 * for and in what order, and URunwaySequencer (M3, AirportOps) will decide who is next
 * before asking here for the runway. A table that knew about aircraft would have to grow
 * with every rule anyone added to traffic.
 *
 * Claims is still the flat array it always was - GetClaims() and every test read it exactly
 * as before - but TryClaim, IsHeld, FindClaim and the per-agent Release* calls no longer
 * walk the whole thing: ByResource and ByAgent (below) index it, so each looks only at the
 * claims on the one resource or by the one agent it cares about. MEASURED, THEN INDEXED
 * (issue #168): with A agents holding k claims each, the un-indexed table cost O(A^2 k^2)
 * compares per arbitration pass, up to 32 times a frame at the top of the speed ladder.
 */
USTRUCT()
struct AIRSIDE_API FTrafficOccupancy
{
	GENERATED_BODY()

	/**
	 * A same-agent claim on the same resource is an UPDATE: checked against everyone else
	 * like a fresh claim, then written over the old one in place. Unconditional replacement
	 * was rejected: a follower whose window grows each tick would re-claim its own edge and
	 * be granted straight through the leader's occupied interval - the pass-through defect
	 * this table exists to end. Preempted agents are recorded for TakePreempted, so the
	 * caller can re-run their claim pass in the same tick rather than let them drive one
	 * frame on a reservation they no longer have.
	 *
	 * THREE OUTCOMES, and bOccupied decides two of them. An existing OCCUPANCY refuses
	 * everything, rank included - nobody is evicted from ground they are standing on, and
	 * two occupants of one node is a Held the caller reports as an overlap. An occupied
	 * CLAIMANT preempts any conflicting reservation whatever the ranks are, because spec
	 * §3.3's promise means a reservation on ground somebody already stands on was never a
	 * claim anyone could act on. Only reservation against reservation is decided by rank,
	 * and there ties keep the holder - which is first-to-reserve.
	 */
	EClaimResult TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker);

	/**
	 * TryClaim for a caller that raises the claim purely for its SIDE EFFECT and drops both
	 * the result and the blocker - four call sites did this by hand, each retyping the same
	 * WHY (#103): a table cannot make an aircraft already committed to occupying this
	 * ground stop, because whoever decided to send it there is what refuses BEFORE it
	 * drives (ArrivalPlanner::Plan for a runway chain; the planner and the rebuild both
	 * skipping a held stand for a goal reservation). A caller that DOES act on Held - a
	 * return value, a log line naming the blocker - keeps calling TryClaim directly.
	 */
	void Assert(const FTrafficClaim& Claim);

	void ReleaseAll(int32 AgentId);

	/**
	 * Drops AgentId's claim on this ONE resource, if it has one. Releasing something nobody
	 * holds is not an error, so a handover that runs twice is harmless.
	 *
	 * NO PRODUCTION CALLER TODAY, and the comment says so rather than implying one. It was
	 * written for the Vacated handover - give the runway back in the tick it was vacated -
	 * and that handover became GEOMETRIC when spec §3.1 grew its fourth route: a landing
	 * that has vacated now hands the chain to FRoadAgent::CrossingRunway and the claim pass
	 * drops it through ReleaseExcept once the tail is clear. Kept because it is the only way
	 * to hand back exactly one resource while an agent goes on holding the rest, which is
	 * what M3's runway sequencer will need, and because Airside.Model.Occupancy.Release
	 * pins the behaviour either way.
	 */
	void Release(int32 AgentId, const FTrafficResource& Resource);

	/**
	 * Drops AgentId's RESERVATIONS and keeps everything it is standing on.
	 *
	 * What a replan gives back (UGroundTraffic::ReplanAt): the line ahead belonged to a
	 * journey nobody is making any more, but where the agent's BODY is is a fact no plan can
	 * change. ReleaseAll was used here and was wrong - a replanned aircraft standing on a
	 * runway showed the strip free to ArrivalPlanner for the frame before its next claim
	 * pass, and a landing could be cleared onto it.
	 */
	void ReleaseReservations(int32 AgentId);

	/** Drops every claim by AgentId whose resource is not in Keep. The per-tick "release
	 *  what is behind me" in one call, so first-to-reserve survives across ticks: an agent
	 *  that released everything and re-claimed would be a stranger to its own queue. */
	void ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep);

	/** Sum of (To - From) over claims on Edge by agents other than ExcludingAgent. */
	double HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const;

	/** True when someone other than ExcludingAgent holds Resource (any interval). */
	bool IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder = nullptr) const;

	/**
	 * True when ANY of Resources IsHeld by someone but Excluding - the runway-chain-held
	 * question ArrivalPlanner and RouteSearch's IsRunwayHeld each hand-rolled as their own
	 * loop (#103): URoadNetwork::RunwaySurfaces(Seed) gives the resource list.
	 *
	 * bCountOwnOccupied adds Excluding's OWN occupied claim to what counts as held - the two
	 * callers disagree on this and both are right for their own question. A landing not yet
	 * dispatched (ArrivalPlanner, Excluding = 0, bCountOwnOccupied = false) has no agent of
	 * its own to be occupying anything. A route already dispatched and asking whether a
	 * runway is safe to use (RouteSearch, bCountOwnOccupied = true) must count its own body:
	 * spec §3.3 says an occupied claim was never a reservation another route could quietly
	 * use, including the one the claim's own owner is currently flying.
	 */
	bool IsAnyHeld(TConstArrayView<FTrafficResource> Resources, int32 Excluding, bool bCountOwnOccupied) const;

	const TArray<FTrafficClaim>& GetClaims() const { return Claims; }

	/**
	 * How many OTHER claims the last TryClaim call compared itself against - issue #168's
	 * own measure that the index, not the agent or claim count, decides the cost. Not reset
	 * automatically: a test reads it right after the TryClaim it means to measure, the way
	 * GetLastStepsForTest works on UGroundTraffic.
	 */
	int32 GetLastTryClaimComparesForTest() const { return LastTryClaimComparesForTest; }

	/**
	 * The one claim AgentId holds on Resource, or null. What the deadlock resolver reads to
	 * tell a blocker that is standing on the ground (bOccupied) from one that has merely
	 * reserved it - the difference between a jam and a yield. See ResolveDeadlocks.
	 */
	const FTrafficClaim* FindClaim(int32 AgentId, const FTrafficResource& Resource) const;

	/**
	 * Drops every EDGE and NODE claim, whoever holds it, and KEEPS every SURFACE claim.
	 *
	 * What a guideline rebuild releases - see UGroundTraffic::OnGraphRebuilt. The distinction
	 * is the whole point of having this beside Clear(). An edge and a node are guideline
	 * handles, and the builder frees every derived one of them: those claims name resources
	 * that have stopped existing, and no agent can re-claim them because the routes naming
	 * them have been re-pointed at other handles. A SURFACE is an FRoadSegmentId - the road
	 * model, which a guideline rebuild does not regenerate at all - so a runway an aeroplane
	 * is standing on is exactly as real after the rebuild as before it.
	 *
	 * DROPPING SURFACE CLAIMS WOULD RE-OPEN THE WINDOW TASK 7 CLOSED. ArrivalPlanner::Plan
	 * reads this table directly at DispatchArrival, BETWEEN ticks, so an aircraft mid-crossing,
	 * rolling out or lined up would show its strip free for as long as it took the player to
	 * click, and a landing could be cleared onto it. Clear() is still the right call for a
	 * session ending; this one is the right call for a graph changing under a running airport.
	 */
	void ReleaseGuidelineClaims();

	/**
	 * The same distinction for ONE agent: drops AgentId's edge and node claims, keeps its
	 * runway surfaces.
	 *
	 * What a STRANDING gives back (UGroundTraffic::ReResolvePlan's Strand, and ClaimAhead's
	 * dead-plan branch). ReleaseAll was used at both and was wrong for the same reason
	 * Clear() was wrong above: an aircraft stranded mid-crossing, or one whose plan went bad
	 * while it stood on the centreline, is still ON the asphalt - a plan says nothing about
	 * where a body is - and ArrivalPlanner::Plan reads this table directly at
	 * DispatchArrival, between ticks. Dropping its strip claim showed the runway free and a
	 * landing could be cleared onto it.
	 *
	 * NOT ReleaseGuidelineClaims: that one is a rebuild, where the resources themselves have
	 * ceased to exist for everybody. This one is one agent giving back the lines it will
	 * never drive, on a graph everyone else is still using.
	 */
	void ReleaseGuidelineClaimsOf(int32 AgentId);

	/** Agents whose reservation was removed by a preemption since the last call; clears. */
	TSet<int32> TakePreempted();

	void Clear();

private:
	/**
	 * Every claim matching Predicate, gone - scanning the WHOLE table, named once behind the
	 * release calls that are not about one agent. ReleaseGuidelineClaims is the one caller
	 * left (issue #168 moved every PER-AGENT release onto ReleaseAgentWhere below, which
	 * reads ByAgent instead of scanning everyone): a rebuild frees resources for every agent
	 * at once, so there is no one bucket to narrow it to.
	 *
	 * Collects matching indices first, then removes them highest-first through
	 * RemoveClaimAtSwap - no longer the one-line Claims.RemoveAllSwap(Predicate) this used
	 * to be, because a bare RemoveAllSwap does not know to keep ByResource and ByAgent true.
	 */
	void ReleaseWhere(TFunctionRef<bool(const FTrafficClaim&)> Predicate);

	/**
	 * The same release, scoped to AgentId's own claims via ByAgent - ReleaseAll, Release,
	 * ReleaseReservations, ReleaseExcept and ReleaseGuidelineClaimsOf all give back one
	 * agent's ground, and an agent holds a handful of claims whatever the airport's size
	 * (issue #168's TrafficOccupancy.cpp:188-193: ReleaseExcept was O(every claim x Keep)).
	 */
	void ReleaseAgentWhere(int32 AgentId, TFunctionRef<bool(const FTrafficClaim&)> Predicate);

	/** Claims.Add(Claim), then indexes the new slot. The only place a claim is APPENDED -
	 *  TryClaim's in-place update (same agent, same resource) skips this because neither
	 *  index key changes. */
	void AddClaim(const FTrafficClaim& Claim);

	/** Adds Claims[Index] - already written - to ByResource and ByAgent. */
	void IndexClaim(int32 Index);

	/** Removes Claims[Index] from ByResource and ByAgent, without touching Claims itself. */
	void UnindexClaim(int32 Index);

	/**
	 * Claims.RemoveAtSwap(Index), keeping ByResource and ByAgent true afterwards.
	 *
	 * RemoveAtSwap moves the LAST claim into Index's slot; done blind, the index maps would
	 * still say that claim lives at the old (now past-the-end) position. This unindexes the
	 * claim being removed, then - if the last claim is about to move - repoints ITS bucket
	 * entries from the old index to the new one BEFORE the array itself changes, which is
	 * why the check has to happen here and not after RemoveAtSwap returns.
	 *
	 * INDICES, NOT A STABLE SLOT ARRAY: RoadSlotMap's bAlive/generation scheme was the other
	 * option issue #168 considered, but it would have made GetClaims() return dead slots -
	 * every test above reads Table.GetClaims().Num() as an exact count, and so does
	 * production code that means to see only what is held right now. Repointing the one
	 * claim a swap actually moves costs the same O(1) either way and keeps Claims exactly
	 * the shape it always was.
	 */
	void RemoveClaimAtSwap(int32 Index);

	UPROPERTY() TArray<FTrafficClaim> Claims;

	/**
	 * Claim indices on one resource, and on one agent - issue #168. Neither is a UPROPERTY,
	 * same reasoning as Preempted below: both hold nothing but int32s derived from Claims,
	 * kept true by every mutator above, so there is nothing here for the collector to trace
	 * or for SaveGame to serialise that Claims does not already own.
	 */
	TMap<FTrafficResource, TArray<int32>> ByResource;
	TMap<int32, TArray<int32>> ByAgent;

	/** Not a UPROPERTY: consumed within the tick that produced it. */
	TSet<int32> Preempted;

	/** See GetLastTryClaimComparesForTest. */
	int32 LastTryClaimComparesForTest = 0;
};
