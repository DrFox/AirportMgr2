#pragma once

#include "CoreMinimal.h"
#include "Model/GroundTraffic.h"

/**
 * One agent's claim pass, extracted off UGroundTraffic (issue #84) so the biggest single
 * concern in that class - "what does this agent hold and reserve this tick, and how far may
 * it go" (spec 2026-09-06 §3) - is a thing a test can construct and drive on its own, rather
 * than a UCLASS full of unrelated dispatch and deadlock machinery.
 *
 * REFERENCES, NOT A COPY OF THE STATE: Rules, Table and Reach are UGroundTraffic's own
 * members, so a claim raised through this struct lands in the SAME occupancy table every
 * other agent's pass (and the deadlock resolver, and dispatch) reads. UGroundTraffic
 * constructs one of these per Arbitrate() call - see its .cpp - rather than holding one as a
 * persistent member, because this struct carries no state of its own between agents: Run
 * leaves nothing behind that the next agent's claim pass would need to see.
 *
 * ClaimAhead's own header (the numbered sequence: the two early branches, the window, step
 * 0's crossing, steps 1-2's wanted list, step 3's ask) is Run's doc comment below - moved
 * with the code it describes, not re-derived.
 */
struct AIRSIDE_API FClaimPass
{
	const FTrafficRules& Rules;
	FTrafficOccupancy& Table;
	FNodeReachCache& Reach;

	/**
	 * ClaimAhead: one agent's whole claim pass for this tick. Spec §3.
	 *
	 * NOT TAXIING: hold the runway and nothing else. An arrival on the roll and a departure
	 * lining up own the strip; whatever either held on the taxiway before the handover is
	 * released here, which is what makes "Vacated releases the chain" fall out of the tick
	 * rather than needing a call of its own.
	 *
	 * A DEAD PLAN releases every guideline claim and keeps the ground the body is on - see
	 * ReleaseForDeadPlan.
	 *
	 * OTHERWISE, the numbered sequence: 0. the runway this agent is physically crossing (spec
	 * §3.1's fourth route - UpdateCrossing); 1 and 2. what the agent wants, in route order,
	 * with nothing yet asked of the table (BuildPending); 3. ask the table, in that order -
	 * the FIRST refusal decides how far the agent may go (ApplyClaims); 4. the stand it is
	 * going to, once the route pass has released what is behind it (ClaimGoalNode).
	 *
	 * WHAT ONE AGENT HOLDS AND RESERVES, AND HOW FAR IT MAY GO, spelled out - spec §3.1-§3.3.
	 *
	 * A Taxiing agent takes T = Travelled, F = footprint, G = gap, and a window
	 * W = Speed^2 / (2 Decel) + G - braking distance plus the gap, so a fast agent reserves
	 * far ahead and a stopped one still holds F/2 + G and keeps its place in a queue. It
	 * then walks its remaining steps from Head = T + W back to Tail = T - F/2 (HALF the
	 * footprint behind, because Travelled is the agent's CENTRE: a van 300 uu past a node
	 * with a 500 uu footprint has cleared it) and asks for, in route order:
	 *
	 *   - the node the current step LEFT, while the centre is still within F/2 of it;
	 *   - on each step the window touches, the edge interval [max(Tail, start),
	 *     min(Head, end)] mapped into edge distance - mirrored through the edge length on a
	 *     reversed step - occupied on the step it is standing on, reserved beyond;
	 *   - that step's END node, once the window passes it; and, by the BOX-JUNCTION ENTRY
	 *     RULE, at the moment the window reaches the START of the FIRST step shorter than
	 *     F + G that the agent has not yet entered. A box is an edge the agent cannot stand
	 *     on without still blocking the node behind it, which is every junction turn path,
	 *     so it must be granted the far end before it commits to the near one. The FIRST
	 *     such step only: a window routinely spans several boxes, and demanding the far end
	 *     of each is the rejected alternative below.
	 *
	 * First refusal in ROUTE ORDER decides everything: WaitingOn is the blocker, BlockedStep
	 * the step, and StopWithin the distance to G short of the refused thing - G short of the
	 * BOX's START when the box was refused at entry, so the agent stops outside the junction
	 * where it can still turn, rather than inside it where nobody can. All granted:
	 * StopWithin unbounded, WaitingOn 0, BlockedStep -1. Past the first refusal the agent
	 * goes on claiming the ground it OCCUPIES and reserves nothing further: dropping its own
	 * occupancies there let the agent that had merely reserved the node it stands on drive
	 * into it.
	 *
	 * TWO ALTERNATIVES REJECTED, both traced by hand on the three-vehicle triangle:
	 *
	 *  - Release everything and re-claim. ReleaseExcept keeps what is still wanted, so
	 *    first-to-reserve survives across ticks; an agent that dropped its holds and asked
	 *    again would be a stranger to its own queue every frame, and any higher-ranked
	 *    agent could step into the gap it had just opened in front of itself.
	 *  - Extending the box requirement through every CONSECUTIVE short step. It deadlocks
	 *    harder: an agent then refuses to move until a node two junctions ahead is free,
	 *    and the agent holding that node is waiting on it. Spec §3.1 records the trace;
	 *    Airside.Model.Traffic.BoxEntryFirstOnly measures it, at 400 uu of line the chained
	 *    rule kept a van out of while the box in front of it was empty.
	 *
	 * A RUNWAY SURFACE IS CLAIMED BY THE TWO TAXIING ROUTES OF SPEC §3.1, both raised in
	 * route order beside the thing that implied them, so the first-refusal rule still decides:
	 *
	 *   - a step whose EDGE derives from a runway segment claims that segment's whole CHAIN,
	 *     occupied on the step being stood on and reserved beyond, ranked as that edge is. A
	 *     refusal stops the agent a gap short of the step's START - outside the strip;
	 *   - a step whose END NODE carries HoldingPositionFor claims the chain that names, always
	 *     RESERVED (nobody is occupied THROUGH a bar) and only once the window has reached
	 *     the node. A refusal stops the agent with its NOSE on the bar, which is the one
	 *     refusal that does not subtract the gap.
	 *
	 * A non-Taxiing agent claims only SURFACES, occupied, and releases the rest: an arrival
	 * on the roll owns the strip and nothing on the taxiway. Which surfaces is RunwayHeld -
	 * the handovers that fill it live in FRoadAgent::Advance - PLUS the chain of any crossing
	 * still in progress, because spec §3.4's "their surface" has to mean the one the body is
	 * on: an aircraft whose plan dies mid-crossing is Parked by the end of that tick with
	 * RunwayHeld empty, and holding nothing would show the strip free with an aeroplane on
	 * it. The third route is that one.
	 */
	void Run(FRoadAgent& Agent, const URoadNetwork& Network);

	/** A non-Taxiing agent's whole claim pass: hold RunwayHeld AND the chain its body is
	 *  crossing, release everything else. Spec §3.4's "their surface and nothing else",
	 *  where the surface includes the one it is standing on. Run's first branch. */
	void HoldRunwayOnly(FRoadAgent& Agent, const URoadNetwork& Network);

	/** A Taxiing agent whose plan went bad under it: give back every GUIDELINE, keep any
	 *  runway surface and the crossing that describes it (a plan says nothing about where a
	 *  body is), clear the arbitration fields. Run's second branch. */
	void ReleaseForDeadPlan(FRoadAgent& Agent);

	/**
	 * The one claim that is not about the ground under or ahead of the agent: its DESTINATION.
	 * An Arriving or Taxiing aircraft RESERVES the stand pose node it is heading for, and a
	 * Parked agent OCCUPIES the node it parked at - stand or not; the M2 rule "a parked agent
	 * holds only its surface" left a parked aircraft on a taxiway junction holding nothing
	 * (spec 2026-09-07-stand-occupancy §3, amended).
	 *
	 * DERIVED FROM GoalNode EVERY TICK, after the phase's own claim pass has run its
	 * ReleaseExcept. Threading the stand through BuildPending/ApplyClaims was rejected: those
	 * are route-ordered claims whose first refusal sets StopWithin, and a stand must never
	 * stop an aircraft short - it is a reservation for a place, not a queue for a line. The
	 * drop-and-reclaim inside one agent's pass is invisible: Arbitrate is synchronous and no
	 * other agent has the same goal (the planner and the rebuild see to that).
	 */
	void ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network);

	/**
	 * The numbers ONE claim pass works in: the agent's centre, its body and gap, and the
	 * stretch of route the window covers. Read by four of the five steps below.
	 *
	 * ONE STRUCT rather than six parameters threaded through four helpers - the codebase's
	 * "one struct per thing", and for its stated reason: a figure copied by hand into a
	 * sibling call is a figure somebody will one day forget to copy, and the copy that
	 * nobody set is how an arrival taxied on default figures.
	 *
	 * The names are the one-letter ones the rules are written in, because the rules are
	 * arithmetic and T + F/2 reads as the nose while Travelled + Footprint/2 does not.
	 */
	struct FClaimWindow
	{
		/** The agent's CENTRE, never its nose - see FClaimPass::CentreOf, which derives it
		 *  from Follower.Travelled and the airframe's own offsets. */
		double T = 0.0;
		/** Footprint and gap for this agent's class. See FTrafficRules. */
		double F = 0.0;
		double G = 0.0;
		/** T + F/2 + braking distance + G, and T - F/2. WindowFor says why the halves. */
		double Head = 0.0;
		double Tail = 0.0;
		/** Index into Plan.Steps that T falls on. */
		int32 Current = INDEX_NONE;
	};

	/**
	 * Where the agent's nose, centre and tail are this tick, sampled ONCE per pass out of
	 * the one array the follower walks. See SampleBody, and UpdateCrossing, which is the
	 * only reader: all three rules of a crossing ask about the same three points, and
	 * sampling per rule is the second evaluator this codebase's guideline invariant forbids.
	 */
	struct FClaimBody
	{
		/** All three or none - PointAtDistance fails on the polyline, not on the distance. */
		bool bValid = false;
		FVector2D Nose = FVector2D::ZeroVector;
		FVector2D Centre = FVector2D::ZeroVector;
		FVector2D Tail = FVector2D::ZeroVector;
	};

	/** One thing an agent wants this tick. Defined in TrafficClaims.cpp, where the only
	 *  three functions that build or read one live. */
	struct FWantedClaim;

	/**
	 * The two maps between ROUTE distance - what the follower walks and what a stop point is
	 * expressed in - and EDGE distance, which is what a claim's interval means. Pure
	 * arithmetic on doubles: no member of this struct, no network, no agent.
	 *
	 * PUBLIC AND NESTED, rather than private statics behind a ForTest forwarder. Both mirror
	 * a reversed step, both are one line of arithmetic that is wrong in a way no fixture
	 * reads back directly (a From > To interval conflicts with nothing at all, silently), so
	 * they are worth pinning on their own and a test has to reach them. Nesting costs nothing.
	 *
	 * Airside.Model.Traffic.ClaimGeometry is the test.
	 */
	struct FClaimGeometry
	{
		/** A claim's extent along one edge, in EDGE distance from that edge's A end. */
		struct FEdgeInterval
		{
			double From = 0.0;
			double To = 0.0;
		};

		/**
		 * The route interval [Lo, Hi] on a step that begins at StepBegin and whose edge is
		 * Length long, mapped into edge distance.
		 *
		 * Route distance to EDGE distance, measured from the edge's A end. A reversed
		 * step is walked from B, so its interval mirrors through the edge length - and
		 * the mirror swaps the ends, which is why From takes what Hi produced. Getting
		 * this backwards would give From > To, and a half-open interval that way round
		 * conflicts with nothing at all.
		 */
		static FEdgeInterval EdgeInterval(double Lo, double Hi, double StepBegin, double Length, bool bReversed)
		{
			FEdgeInterval Interval;
			Interval.From = Lo - StepBegin;
			Interval.To = Hi - StepBegin;
			if (bReversed)
			{
				Interval.From = Length - (Hi - StepBegin);
				Interval.To = Length - (Lo - StepBegin);
			}
			return Interval;
		}

		/**
		 * Where a blocker's edge interval first bars the way, back in ROUTE distance, on a
		 * step that begins at StepBegin and whose edge is Length long.
		 *
		 * The blocker's NEAREST boundary ahead, back in route distance. On a reversed
		 * step the agent is walking the edge from B, so the near end of the blocker's
		 * interval is its To, mirrored.
		 */
		static double BoundaryAhead(double StepBegin, double Length, bool bReversed,
			double BlockerFrom, double BlockerTo)
		{
			return bReversed ? StepBegin + (Length - BlockerTo) : StepBegin + BlockerFrom;
		}
	};

	/** T, F, G, Head, Tail and the current step for one pass. See FClaimWindow. */
	FClaimWindow WindowFor(const FRoadAgent& Agent) const;

	/** Nose, centre and tail out of Plan.Polyline, once. See FClaimBody. */
	static FClaimBody SampleBody(const FRoutePlan& Plan, const FClaimWindow& Window);

	/** Step 0's phase machine: arm at a bar that leads onto the strip, note the centre
	 *  reaching it, release when the whole body is off it. Spec §3.1's fourth route. */
	void UpdateCrossing(FRoadAgent& Agent, const URoadNetwork& Network,
		const FClaimWindow& Window, const FClaimBody& Body) const;

	/** Steps 0-2: everything the agent wants this tick, IN ROUTE ORDER, which is what the
	 *  first-refusal rule reads. Nothing is asked of the table here. */
	void BuildPending(const FRoadAgent& Agent, const URoadNetwork& Network,
		const FClaimWindow& Window, TArray<FWantedClaim>& Pending) const;

	/** Step 3: ask the table for each in turn, keep what was granted or occupied, and let
	 *  the FIRST refusal write StopWithin, WaitingOn and BlockedStep. */
	void ApplyClaims(FRoadAgent& Agent, const FClaimWindow& Window, const TArray<FWantedClaim>& Pending);

	/** How far a refused claim lets the agent go, in route distance from T. A pure function
	 *  of the refusal's kind, the step it was raised on, and the window. */
	static double StopWithinFor(const FWantedClaim& Want, const FTrafficClaim& Blocker,
		const FClaimWindow& Window);

	/**
	 * Where this agent's BODY CENTRE is along its route, uu.
	 *
	 * NOT Follower.Travelled, which measures the STEERED AXLE, and not the origin it is
	 * derived from either - on a conforming airframe that origin is the nose gear. The
	 * window below is written as T +/- F/2 about the centre, so taking Travelled for it put
	 * the whole window half a length forward and left the agent's own tail unclaimed. That
	 * was approximately true while mesh origins sat mid-fuselage and stopped being true
	 * when plane2 was re-exported about its nose gear on 2026-09-13.
	 *
	 * Both offsets are per-type data, so an airframe with neither measured returns Travelled
	 * and claims exactly the line it claims today - which is every service vehicle.
	 *
	 * Static and taking the agent so a test can ask it one question with no claim pass, no
	 * graph and no world.
	 */
	static double CentreOf(const FRoadAgent& Agent);

	/**
	 * Who goes first at Node. The node's PriorityOverride if it has one, else the class
	 * order. Spec §3.3, §5.4.
	 *
	 * STATIC, taking Reach explicitly: FDeadlockResolver::CanReplanAtBlockedStep needs
	 * ReachExcessAt below without owning a claim pass of its own, and a static free of any
	 * instance is the plainest way to share one function between the two rather than
	 * standing up a throwaway FClaimPass just to call it.
	 */
	static int32 RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class);

	/**
	 * How much FURTHER than half a footprint Node's claim reaches along Edge for this class,
	 * in uu; 0 at an ordinary junction. The claim pass adds it wherever it used to compare a
	 * distance to the node against F/2, and the stop point for a refused node moves back by
	 * it, so a body told to wait for a node waits where the lines have actually parted.
	 */
	static double ReachExcessAt(const FTrafficRules& Rules, FNodeReachCache& Reach, const URoadNetwork& Network,
		FGuidelineNodeId Node, FGuidelineEdgeId Edge, ETraversalClass Class);
};
