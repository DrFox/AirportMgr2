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
	/** One segment of a runway. A landing holds every segment of the chain; a hold-short
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
 * A flat array searched linearly rather than a map per kind: an airport has tens of agents
 * holding a handful of claims each, and the whole table is rebuilt in one pass per tick.
 * Measured before it is indexed.
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

	const TArray<FTrafficClaim>& GetClaims() const { return Claims; }

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

	/** Agents whose reservation was removed by a preemption since the last call; clears. */
	TSet<int32> TakePreempted();

	void Clear();

private:
	UPROPERTY() TArray<FTrafficClaim> Claims;

	/** Not a UPROPERTY: consumed within the tick that produced it. */
	TSet<int32> Preempted;
};
