// FClaimPass and the helpers it alone uses - spec 2026-09-06 §3.1-§3.3.
//
// PULLED OFF UGroundTraffic BY ISSUE #84: this was UGroundTraffic::ClaimAhead and its
// private helpers, in GroundTrafficClaims.cpp, one class across four translation units (the
// shape URoadEditFacade already uses). It is a struct of its own now - Rules, Table (the
// occupancy table, was UGroundTraffic::Occupancy) and Reach (was UGroundTraffic::NodeReach)
// are references to the SAME objects UGroundTraffic still owns, handed in by
// UGroundTraffic::Arbitrate. See Model/TrafficClaims.h for the struct itself.
//
// Run itself is the numbered sequence its comments have always carried: the two early
// branches, the window, step 0's crossing, steps 1-2's wanted list, step 3's ask. Each
// numbered step is one function below, and Run is last.

#include "Model/TrafficClaims.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

// GroundTraffic.h ABOVE IS FOR UGroundTraffic::StepStart/StepFromNode/CurrentStep ONLY
// (issue #175): those three static helpers stayed on UGroundTraffic - see its header - so
// this .cpp needs the full class now that TrafficClaims.h itself no longer pulls it in.

/**
 * One thing an agent wants this tick, plus what the REFUSAL rule needs to know about it.
 *
 * The step geometry travels with the claim rather than being re-derived at the refusal,
 * because the refusal has to answer "how far may this agent go" in ROUTE distance while
 * the claim itself is in EDGE distance, and the map between them (which step, how long,
 * which way round) is exactly these fields. Re-deriving it from the blocker would mean a
 * second reading of the same step - the thing this codebase calls a second evaluator.
 */
struct FClaimPass::FWantedClaim
{
	/**
	* Which of spec §3.1's routes to the runway surface raised this claim, when it was
	* one of them. An ENUM and not two bools: the two can never both be true, and the
	* refusal below has to pick exactly one stop-point rule from it - see the codebase's
	* "a phase is an enum, never a set of bools".
	*/
	enum class ESurface : uint8
	{
		/** Not a surface claim at all - an edge or a node. */
		None,
		/** The step's edge derives from a runway: stop a GAP short of the edge's start,
		*  outside the strip. */
		RunwayEdge,
		/** The step's end node carries a hold bar: stop with the NOSE on the bar. */
		HoldingPosition,
	};

	FTrafficClaim Claim;

	/** Index into Plan.Steps this claim was raised for. Becomes FRoadAgent::BlockedStep. */
	int32 Step = INDEX_NONE;

	/** True for the node at the END of Step; false for an edge or the node it left. */
	bool bEndNode = false;

	/** True when Step is shorter than Footprint + Gap AND the agent has not entered it -
	 *  the box-junction entry case, which stops the agent short of the box's START. */
	bool bBoxEntry = false;

	double StepStart = 0.0;
	double StepEnd = 0.0;
	double EdgeLength = 0.0;
	bool bReversed = false;

	ESurface Surface = ESurface::None;

	/**
	 * For an end-node claim: how far short of the node the lines have parted, beyond the
	 * half footprint - FClaimPass::ReachExcessAt. Carried here so StopWithinFor can stay
	 * a pure function of the claim, without the network.
	 */
	double ReachExcess = 0.0;

	/** The node carrying the bar, for the log line. Set only when Surface == HoldingPosition;
	*  the segment it protects is already on Claim.Resource. */
	FGuidelineNodeId HoldNode;

	/**
	 * A CROSSING'S ENTRY HOLD (taxiway strip stage 4, final review): where the agent's CENTRE
	 * stops, in route distance, if this claim - one raised inside a road-taxiway crossing's
	 * chain of split pieces - is refused before the agent has entered the chain. Negative when
	 * no such hold applies. HoldStep is the step leaving the entry, for FRoadAgent::BlockedStep.
	 * See FClaimPass::BuildPending's step 0'' for the two entries (a road's stop line, an
	 * aircraft's box entry) and why a refusal anywhere in the chain holds there.
	 */
	double HoldAt = -1.0;
	int32 HoldStep = INDEX_NONE;

	/**
	 * Raised inside (or at the approach to) a crossing chain the agent has not yet entered or
	 * committed to. ONE BOX IS ALL OR NOTHING (re-review, important 4): refused anywhere in it,
	 * the agent gives back every reservation it was granted in it this pass - see ApplyClaims.
	 */
	bool bInChain = false;

	/**
	 * The same all-or-nothing, for a RUNWAY crossing's exit chain (BuildPending's step 0''').
	 * A SECOND FLAG AND NOT A SECOND USE OF bInChain (review, 2026-09-30): the two boxes can be in
	 * one window - an aircraft inside a road-taxiway crossing with a runway bar ahead - and each
	 * gives back only its OWN grants. Shared, a refused runway exit handed back the road box's
	 * conflict reservations under an aircraft still driving through it, for a van to take. Two
	 * bools and not an enum: a claim on the step between the boxes can be in both.
	 */
	bool bInExitChain = false;

	/**
	 * Set on a claim raised inside a RUNWAY crossing's exit chain (BuildPending's step 0''') -
	 * the ENTRY bar the agent holds at if it is refused, for ApplyClaims' log line. Unset
	 * everywhere else; a handle and not a bool beside HoldNode because the line names the bar.
	 */
	FGuidelineNodeId CrossingBar;
};

namespace
{
	/**
	 * Does route step Index lead ONTO the strip Chain describes? ONE predicate for the two places
	 * that ask it - UpdateCrossing's arming test (0a) and BuildPending's bar and exit rules - so an
	 * entry bar is the same bar to both (review of dedf049f: they disagreed, and a bar the claim
	 * pass called an exit reserved nothing while the arming test still put the agent on the strip).
	 *
	 * THE STEP'S END NODE, then its polyline: every vertex, and the line BETWEEN them sampled at the
	 * strip's half width. A straight line across a strip spends at least a full width on it, so at
	 * half-width spacing one sample must land inside. The hand-drawn bar-to-bar edge is the case:
	 * two vertices, both bars, both off the asphalt by construction - and a midpoint that is off it
	 * too once the two set-backs differ by more than the strip's width (AsymmetricBarToBarHolds).
	 *
	 * READ OFF Plan.Polyline, the array the follower walks - the sample-once rule. Capped at 256
	 * samples: a crossing step is a few strip widths long (2-10 samples on 2026-09-30's maps), and
	 * the cap only bounds a pathological hand-drawn edge, whose far reaches are not a crossing.
	 *
	 * FROM FromDistance ON (route distance; the step's start when negative). UpdateCrossing asks
	 * from the TAIL: on the bar-to-bar edge the step still leaving the near bar is the step the
	 * agent is on after the geometric release, and asked whole it "leads onto the strip" behind the
	 * aircraft and re-armed the crossing (BarToBarCrossing: "the bar on the way OUT arms nothing").
	 */
	bool StepLeadsOntoStrip(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index,
		const TArray<FRoadSegmentId>& Chain, double FromDistance = -1.0)
	{
		if (!Plan.Steps.IsValidIndex(Index))
		{
			return false;
		}
		if (Network.IsGuidelineNodeOnRunway(Plan.Steps[Index].To, Chain))
		{
			return true;
		}
		const double End = Plan.Steps[Index].EndDistance;
		const double Start = FMath::Max(UGroundTraffic::StepStart(Plan, Index), FromDistance);
		if (Start >= End)
		{
			return false;
		}
		double HalfWidth = 0.0;
		const int32 FirstVertex = Index > 0 ? Plan.Steps[Index - 1].EndVertex : 0;
		const int32 LastVertex = FMath::Min(Plan.Steps[Index].EndVertex, Plan.Polyline.Num() - 1);
		double VertexAt = UGroundTraffic::StepStart(Plan, Index);
		for (int32 Vertex = FMath::Max(0, FirstVertex); Vertex <= LastVertex; ++Vertex)
		{
			if (Vertex > FMath::Max(0, FirstVertex))
			{
				VertexAt += FVector2D::Distance(Plan.Polyline[Vertex - 1], Plan.Polyline[Vertex]);
			}
			// Asked of every vertex for the half width; counted only from Start on.
			if (Network.IsPointOnRunway(Plan.Polyline[Vertex], Chain, &HalfWidth) && VertexAt >= Start)
			{
				return true;
			}
		}
		if (HalfWidth <= 0.0)
		{
			return false;
		}
		constexpr int32 MaxSamples = 256;
		const int32 Samples = FMath::Clamp(FMath::CeilToInt((End - Start) / HalfWidth), 1, MaxSamples);
		int32 HintVertex = 1;
		double HintWalked = 0.0;
		for (int32 K = 1; K < Samples; ++K)
		{
			FVector2D Point;
			double Heading = 0.0;
			if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Start + (End - Start) * K / Samples, Point, Heading,
				HintVertex, HintWalked))
			{
				return false;
			}
			if (Network.IsPointOnRunway(Point, Chain))
			{
				return true;
			}
		}
		return false;
	}
}

void FClaimPass::HoldRunwayOnly(FRoadAgent& Agent, const URoadNetwork& Network)
{
	// CLEARED, or the arbitration fields say whatever they said on the last tick this
	// agent taxied. A parked aircraft still naming the vehicle it once queued behind
	// would feed Task 8's wait-for graph an edge out of an agent that is not waiting for
	// anything, and a cycle through it would be a phantom nobody could resolve.
	Agent.ClearArbitration();
	Agent.SetLastOverlaps({});

	// MEMBER, NOT A LOCAL (issue #190) - see the header. Reset here, not left with whatever
	// the previous non-Taxiing agent this pass built.
	Surfaces.Reset();
	Surfaces.Reserve(Agent.RunwayHeld.Num() + 1);
	for (const FRoadSegmentId Segment : Agent.RunwayHeld)
	{
		Surfaces.Add(FTrafficResource::OfSurface(Segment));
	}

	// AND THE SURFACE THE BODY IS STANDING ON, which is spec §3.4's "other phases hold
	// their surface and nothing else" read the only way that is true of a physical
	// aeroplane: the surface a NON-TAXIING agent holds is RunwayHeld plus whatever its
	// body is on, and a crossing is exactly the second of those.
	//
	// A CROSSING WAS TREATED AS A TAXIING IDEA HERE AND CLEARED, which was wrong for the
	// one case that matters: an aircraft whose plan dies mid-crossing is Stranded by the end
	// of that same tick (an invalid plan makes FRouteFollower::HasArrived true), and
	// RunwayHeld is empty on anything that did not land - so the strip under it came free
	// on the next tick and a landing could be cleared onto an aeroplane standing on the
	// centreline. Keeping it costs nothing anywhere else: UpdateCrossing releases a
	// crossing geometrically while the agent is still Taxiing, so an agent that reaches
	// this branch with the phase still set is one whose body genuinely never left the
	// asphalt. The fields are reset only where the agent itself goes - RetireAgent and
	// Advance's removal path, both of which ReleaseAll - which is the player's decision,
	// and the log line that announced a release lives at the geometric release, where the
	// release actually happens.
	if (Agent.CrossingPhase != ECrossingPhase::None && Agent.CrossingRunway.IsSet())
	{
		// THE WHOLE CHAIN, re-expanded per tick exactly as BuildPending's route zero does
		// it: a runway is several segments once it has exits, and a rebuild may have
		// changed which - so the seed is what is stored and the chain is what is claimed.
		// THROUGH Chains, not Network.RunwayChainOrSeed (issue #170): "re-expanded per tick"
		// only ever meant "answered fresh if the graph could have changed it", and Chains
		// gives that for a map lookup instead of a walk on every one of the substeps a
		// parked or rolling aircraft spends holding its own surface.
		const TArray<FRoadSegmentId>& Chain = Chains.GetOrSeed(Network, Agent.CrossingRunway);
		for (const FRoadSegmentId Segment : Chain)
		{
			Surfaces.AddUnique(FTrafficResource::OfSurface(Segment));
		}
	}

	Table.ReleaseExcept(Agent.Id, Surfaces);

	// See FTrafficOccupancy::Assert for the WHY every fire-and-forget claim here shares:
	// an aircraft already rolling cannot be told to stop by a table, and DispatchArrival
	// refused the landing at the door if the chain was held (see ArrivalPlanner::Plan). Who
	// is allowed onto a runway NEXT is URunwaySequencer's question in M3, asked before
	// anything is dispatched.
	for (const FTrafficResource& Resource : Surfaces)
	{
		Table.Assert(FTrafficClaim::Make(Agent.Id, Resource, /*bOccupied*/ true, TraversalPriority(Agent.Class)));
	}
}

void FClaimPass::ReleaseForDeadPlan(FRoadAgent& Agent)
{
	// NO LINE LEFT TO WALK, SO NO LINE HELD. Released rather than left alone: an agent
	// whose plan was replaced by an invalid one would otherwise keep its last edge and
	// node claims for the rest of the session, blocking a junction nobody could ever be
	// told was free.
	//
	// AND ITS RUNWAY SURFACES STAY - ReleaseGuidelineClaimsOf, not ReleaseExcept(nothing),
	// which is what this called. A PLAN SAYS NOTHING ABOUT WHERE A BODY IS: an aeroplane
	// whose route dies while it is standing on the centreline is still standing on the
	// centreline, and ArrivalPlanner::Plan reads this table directly at DispatchArrival,
	// BETWEEN ticks - so giving the strip back cleared a landing onto it. The same
	// correction as ReplanAt's (reservations only) and the rebuild's Strand, for the same
	// reason: only the things a ROUTE names are a route's to give back.
	Table.ReleaseGuidelineClaimsOf(Agent.Id);

	// THE CROSSING FIELDS SURVIVE WITH THE CLAIM THEY DESCRIBE. Clearing them here (which
	// is what this did, in the breath that also dropped the claim) would leave the phase
	// saying the agent is off the strip while the table says it is on it - and the release
	// line it logged would have been a log that lies.
	//
	// AND THEY GO ON SURVIVING. An invalid plan makes FRouteFollower::HasArrived true, so
	// this agent is Stranded by the end of this very tick and takes HoldRunwayOnly from the
	// next one - which re-claims the crossing chain for exactly this reason. The hold ends
	// where the agent does: RetireAgent, or Advance's removal path. A player who deletes the
	// taxiway under a crossing aeroplane has an aeroplane on the runway, and the table says
	// so until they retire it.

	Agent.ClearArbitration();
	Agent.SetLastOverlaps({});
}

double FClaimPass::CentreOf(const FRoadAgent& Agent)
{
	// ALONG THE ROUTE rather than along the body axis, and the approximation is deliberate:
	// on a bend the two differ by well under a centimetre at these offsets, and the exact
	// form would need the heading here - turning a distance into a pose, and this function
	// into a second evaluator of where the agent is. See the guideline invariant.
	//
	// THE SIGN IS THE PHASE'S, and this is the half that was missing. Ahead is NEGATIVE on a
	// conforming airframe - the plan centre sits aft of the nose gear - so it places the
	// centre BEHIND the steered axle, which is right while the nose gear leads. A push
	// reverses which end leads: the aeroplane is pulled out along a lead-in running away from
	// the terminal it faces, so the MAINS lead and the body is ahead of the nose gear in plan
	// distance. Keeping the taxi's sign misplaces the claimed body by twice the offset, 12.6 m
	// on plane2 - an aeroplane still holding a stand it has left, pushing into ground it has
	// not claimed, and nothing in any log to say so. Airside.Model.ClaimCentre pins both ways.
	//
	// AND THE DISTANCE COMES FROM WHICHEVER STRUCT IS DRIVING - see FRoadAgent::
	// DistanceAlongPlan. Agent.Follower.Travelled on a manoeuvring agent is where the taxi IN
	// ended, which measured 99 000 uu against a real 1 500 the first time it was asked.
	//
	// A REVERSE FLIPS IT FOR THE SAME REASON (issue #434). A vehicle backing along its plan has the
	// body's forward pointing AGAINST the plan, so the steered axle - which DistanceAlongPlan
	// reports, see FRoadAgent::ReverseProgress - is the TRAILING one of the two axles in plan
	// distance and the body centre, aft of it, is further along.
	// ENFORCED BY: Airside.Model.ClaimCentre (a reversing body's centre, measured from its fixed axle)
	const double Ahead = Agent.Chassis().BodyCentreX - Agent.Chassis().SteerAxleX;
	const bool bBodyBacks = Agent.Phase == EAgentPhase::Manoeuvring || Agent.Phase == EAgentPhase::Reversing;
	const double Sign = bBodyBacks ? -1.0 : 1.0;
	return Agent.DistanceAlongPlan() + Sign * Ahead;
}

FClaimPass::FClaimWindow FClaimPass::WindowFor(const FRoadAgent& Agent) const
{
	const FRoutePlan& Plan = Agent.PlanInProgress();

	// THE CENTRE, which is no longer Follower.Travelled - see CentreOf. It was, back when
	// every mesh origin sat mid-fuselage; plane2's re-export about its nose gear made the
	// two differ by 6.3 m and moved this window silently with it.
	const double T = CentreOf(Agent);
	const double F = Rules.FootprintFor(Agent.Class);
	const double G = Rules.GapFor(Agent.Class);

	// The airframe's own braking figure, not the rules': the window has to be the distance
	// THIS airframe needs, or an agent reserves less line than it can stop in. Read off
	// Agent.Chassis() now (issue #83; the chassis alone since 2026-09-23) - the follower no
	// longer keeps its own copy of it.
	const double Decel = FMath::Max(KINDA_SMALL_NUMBER, Agent.Chassis().Ground.Taxi.Decel);
	//
	// AND THE SPEED IS THE DRIVING PHASE'S. A push runs at a metre or two a second and brakes
	// on its own PushAccel, so this reserves rather more line than it strictly needs - which
	// is conservative in the safe direction and deliberately left that way: the window is
	// about the room an agent RESERVES, and a push that under-reserved could be cleared into.
	// Do not "fix" this to FPushbackRun::PushAccel without deciding that question first.
	const double Speed = Agent.SpeedAlongPlan();
	const double Window = Speed * Speed / (2.0 * Decel) + G;

	// HALF the footprint each way, because Travelled is the CENTRE, and the window is
	// measured from the NOSE - which is what FTrafficRules::AircraftGap already says the gap
	// is ("clear line kept ahead of the nose"), and what makes spec §3.2's "a stopped agent
	// still holds Footprint + Gap" arithmetically true: F/2 + F/2 + G.
	//
	// THE REJECTED ALTERNATIVE IS Head = T + Window, spec §3.1's wording read without §3.2.
	// It is not cosmetic. A stopped agent then holds exactly up to the boundary it was told
	// to stop G short of - the two TOUCH, half-open intervals do not conflict when they
	// touch, so it is GRANTED, accelerates, grows its window by the very next tick, is
	// refused, brakes to a stop, and is granted again, for ever. Measured, both ways, on the
	// head-on and node-yield fixtures: 2496 "Agent N resumes" lines in one test run without
	// the F/2, against 4 in the whole 99-test suite with it. That flicker also reset
	// StalledSeconds every other tick, which would have left Task 8's deadlock detection
	// unable to see a single stalled agent - the bug would have surfaced two tasks later,
	// as a resolver that never fires.
	double Head = T + F * 0.5 + Window;
	const double Tail = T - F * 0.5;

	// A PUSH HOLDS THE WHOLE MANOEUVRE, not just the room it needs to stop in. That is what
	// "granted whole" has to mean, and checking the ground once at DepartAgent was not enough
	// on its own: an aeroplane already taxiing in reserved its way into the lead-in a tick
	// later, and the two then held each other - "Agent 1 stops 8431 uu short of node 6 held by
	// agent 2" against "Agent 2 stops 2995 uu short of edge 4 held by agent 1", in
	// Airside.Model.Traffic.DepartureMeetsArrivalOnTaxiway.
	//
	// A HEAD-ON THE RESOLVER CANNOT BREAK, which is the whole reason this matters. Every other
	// deadlock in this model is broken by somebody replanning; a manoeuvring agent has no
	// second way off its stand, so it has no move to offer and the jam is permanent. Holding
	// the manoeuvre from the moment it is cleared is what stops the jam forming: the other
	// aeroplane is refused while the push is still short, waits, and goes when it is over.
	//
	// MEASURED FROM THE CENTRE like everything else here, so the half footprint and the gap
	// are added for the same reasons the ordinary Head adds them.
	if (Agent.Phase == EAgentPhase::Manoeuvring)
	{
		Head = FMath::Max(Head, Agent.Pushback.Plan.Length + F * 0.5 + G);
	}

	// A REVERSE HOLDS ITS WHOLE SPAN FOR THE SAME REASON (issue #434), and it is the same kind of
	// manoeuvre: a vehicle backing along a bay's reverse leg has no second way off it, so a head-on
	// there has nobody to replan out of it and is a jam the resolver cannot break. Until this branch
	// it held nothing at all - IsOnRoute left the phase out, so every pass gave it HoldRunwayOnly -
	// and a push was cleared into a truck backing over the same ground
	// (Airside.Model.Traffic.PushRefusedIntoABackingTruck).
	//
	// TO THE SPAN'S END, NOT THE ROUTE'S. What the vehicle drives after backing out is an ordinary
	// taxi and claims as one from the tick it starts. The half footprint and the gap are added as
	// for the push and the ordinary head above - see the alternative rejected there.
	else if (Agent.Phase == EAgentPhase::Reversing)
	{
		Head = FMath::Max(Head, Agent.ReverseSpanEnd() + F * 0.5 + G);
	}
	const int32 Current = UGroundTraffic::CurrentStep(Plan, T);

	// COPIED OUT ONE AT A TIME rather than assigned as the arithmetic runs: every line
	// above is the line it was inside ClaimAhead, so the figures can be diffed against
	// the version this replaced without reading past a rename.
	FClaimWindow Out;
	Out.T = T;
	Out.F = F;
	Out.G = G;
	Out.Head = Head;
	Out.Tail = Tail;
	Out.Current = Current;
	return Out;
}

FClaimPass::FClaimBody FClaimPass::SampleBody(const FRoutePlan& Plan, const FClaimWindow& Window)
{
	const double T = Window.T;
	const double F = Window.F;

	// THE BODY, NOT THE NODES. Nose, centre and tail as POSITIONS, read out of the ONE
	// array the follower walks (GuidelineGeom::PointAtDistance over Plan.Polyline) so the
	// hold cannot disagree with where the agent visibly is - the sample-once rule.
	//
	// THE NODE-BASED VERSION THIS REPLACES was right only for a crossing that HAS a node
	// on the strip, which the generated graph guarantees and a hand-drawn bar-to-bar edge
	// does not: on one edge straight across, it armed half a footprint late and released
	// with up to half a footprint of tail still on the asphalt. Spec §3.1's fourth route
	// exists to prevent exactly that, so the geometry it is written in has to be the
	// body's, not the graph's.
	//
	// All three or none: PointAtDistance fails only for a polyline too short to have a
	// direction, which is a property of the array and not of the distance asked for - so
	// re-ordering which of the three is tried first (below) cannot change bValid.
	//
	// WALKED TAIL, CENTRE, NOSE - ASCENDING - rather than the order the fields are named,
	// so the hint (issue #190) can carry forward through all three instead of three
	// independent scans of Plan.Polyline from vertex 0 for every agent every substep. F is
	// a footprint and never negative, so Tail <= Centre <= Nose always; the hint's own
	// Distance<Walked guard would catch it if that ever stopped holding, at the cost of one
	// full scan rather than a wrong answer.
	FClaimBody Body;
	double Bearing = 0.0;
	int32 HintVertex = 1;
	double HintWalked = 0.0;
	Body.bValid =
		GuidelineGeom::PointAtDistance(Plan.Polyline, T - F * 0.5, Body.Tail, Bearing, HintVertex, HintWalked)
		&& GuidelineGeom::PointAtDistance(Plan.Polyline, T, Body.Centre, Bearing, HintVertex, HintWalked)
		&& GuidelineGeom::PointAtDistance(Plan.Polyline, T + F * 0.5, Body.Nose, Bearing, HintVertex, HintWalked);
	return Body;
}

void FClaimPass::UpdateCrossing(FRoadAgent& Agent, const URoadNetwork& Network,
	const FClaimWindow& Window, const FClaimBody& Body) const
{
	const FRoutePlan& Plan = Agent.PlanInProgress();
	const double T = Window.T;
	const double F = Window.F;
	const int32 Current = Window.Current;

	// UNDER THE NAMES THE THREE RULES BELOW WERE WRITTEN IN. Aliases and not a rename:
	// all three rules read the SAME sample (see FClaimBody), and every line of them is
	// the line it was when it sat inside ClaimAhead.
	const bool bHaveBody = Body.bValid;
	const FVector2D& NosePoint = Body.Nose;
	const FVector2D& CentrePoint = Body.Centre;
	const FVector2D& TailPoint = Body.Tail;

	const FGuidelineNodeId FromId = UGroundTraffic::StepFromNode(Plan, Current);
	const FGuidelineNode* FromNode = Network.GetGuidelineNode(FromId);

	// RUNWAY holding positions only: HoldingPositionFor is set iff the node is one. An
	// INTERMEDIATE position protects nothing and is inert here until M3's sequencer
	// issues instructions at it (spec 2026-09-07).
	const bool bFromIsBar = FromNode != nullptr && FromNode->HoldingPositionFor.IsSet();
	// 0a. COMMITTED: past a bar, on a step that leads ONTO the strip.
	//
	// THE THREE WAYS A STEP CAN LEAD ONTO THE STRIP, tried in that order because they cost
	// that much: the step's END NODE is on it (the generated crossing, whose middle node
	// sits on the centreline); some polyline VERTEX of this step is on it (a bend that
	// crosses); or the NOSE is on it already (the hand-drawn bar-to-bar edge, where the
	// first two find nothing because the only vertices are the two bars, both off the
	// asphalt by construction). The last is the body test, and it is why the arming moment
	// on such a crossing is "a wheel reaches the runway" rather than "a node says so".
	// SINCE 2026-09-30 the first two are StepLeadsOntoStrip, which also samples the line
	// between the vertices - so the bar-to-bar edge arms at the bar, and the nose test is the
	// fallback for a polyline that cannot be sampled.
	//
	// AND THIS IS WHAT TELLS AN ENTRY FROM AN EXIT - spec §3.1, refined during Task 7. A
	// crossing is painted with a bar on EACH side, so the far bar is also "a bar the agent
	// has just passed"; arming there re-armed the hold on the way out and kept the runway
	// occupied behind an aircraft that had already crossed, for the whole exit leg
	// (measured at 17000 uu, i.e. the hold never ended). The exit bar's step leads away
	// and its body is clear, so all three tests answer no and it arms nothing.
	if (Agent.CrossingPhase == ECrossingPhase::None && bFromIsBar)
	{
		const FRoadSegmentId Bar = FromNode->HoldingPositionFor;

		// WALKED ONCE FOR THE WHOLE ARMING TEST (issue #170): the three tries below used to
		// ask Network.IsGuidelineNodeOnRunway/IsPointOnRunway(Position, Bar) separately, each
		// re-walking RunwayChain(Bar) to answer the same "which segments is Bar protecting"
		// question the try before it had just answered. Chains.Get answers it once per Bar
		// per revision; every one of these calls after the first is a map lookup.
		const TArray<FRoadSegmentId>& BarChain = Chains.Get(Network, Bar);

		// THE SAME PREDICATE BuildPending's bar and exit rules ask (review of dedf049f): end node,
		// vertices, and the line between them. Two tests that must agree about which bar is an
		// entry are one test - see StepLeadsOntoStrip. On the hand-drawn bar-to-bar edge this now
		// arms as the centre passes the bar, as a generated crossing does, rather than half a
		// footprint later when the nose reaches the asphalt; that closes the stretch in between
		// where the agent was neither crossing nor behind an entry bar, and held nothing.
		bool bOntoStrip = StepLeadsOntoStrip(Network, Plan, Current, BarChain, T - F * 0.5);

		if (!bOntoStrip)
		{
			// FALLBACK ON LastMotion only when the polyline cannot be sampled at all. It
			// is a tick stale, which is why it is not the first choice.
			bOntoStrip = bHaveBody
				? Network.IsPointOnRunway(NosePoint, BarChain)
				: Network.IsPointOnRunway(Agent.LastMotion.Position, BarChain);
		}

		if (bOntoStrip)
		{
			Agent.BeginCrossing(Bar, ECrossingPhase::Committed);
		}
	}

	// 0b. ON THE STRIP once the CENTRE is - checked every tick while Committed, and on
	// whatever step the agent has reached by then: a bar may sit a step or more short of
	// the asphalt, and the crossing does not begin at the bar, it begins at the edge of
	// the surface.
	if (Agent.CrossingPhase == ECrossingPhase::Committed && Agent.CrossingRunway.IsSet()
		&& bHaveBody && Network.IsPointOnRunway(CentrePoint, Chains.Get(Network, Agent.CrossingRunway)))
	{
		// SAME SEED, NEW PHASE: BeginCrossing again rather than the phase alone, so the pair
		// is written together even on this advance-in-place transition.
		Agent.BeginCrossing(Agent.CrossingRunway, ECrossingPhase::OnStrip);
	}

	// 0c. RELEASED once the TAIL is off the strip, and not one metre before.
	//
	// REJECTED: releasing at the next route node. A runway node sits ON the strip - the
	// crossing's own middle node is the obvious case - so that hands the runway back with
	// half the aeroplane still on it, which is the very frame this rule exists for.
	//
	// REJECTED: waiting for a bar on the far side. A player may place one bar, or none,
	// and a hold that waits for a bar that does not exist never ends.
	if (Agent.CrossingPhase != ECrossingPhase::None && Agent.CrossingRunway.IsSet())
	{
		// ONE WALK FOR ALL FOUR TESTS BELOW (issue #170): nose, centre, tail and the node
		// fallback all ask the same seed, Agent.CrossingRunway - a chain this cache has
		// almost certainly already answered for this tick, since 0a/0b above name the same
		// seed once it is set.
		const TArray<FRoadSegmentId>& CrossingChain = Chains.Get(Network, Agent.CrossingRunway);

		bool bClear = false;
		if (bHaveBody)
		{
			// NO PART OF THE BODY STILL ON IT - nose, centre and tail. "The tail is off
			// the strip" ALONE is not the test and was tried: it is true on the way IN as
			// well, so the hold ended 5000 uu early, with the aeroplane about to drive
			// onto the asphalt (measured at route 17825 on the two-bar crossing, where the
			// tail is off the strip while the nose is already on it).
			const bool bBodyClear =
				!Network.IsPointOnRunway(NosePoint, CrossingChain)
				&& !Network.IsPointOnRunway(CentrePoint, CrossingChain)
				&& !Network.IsPointOnRunway(TailPoint, CrossingChain);

			// WHICH SIDE the body is clear of is what the phase answers. OnStrip means the
			// centre has been on the asphalt, so a clear body is one that has crossed.
			// Committed with a clear body is only meaningful once the agent has left the
			// step out of the bar: a bar whose step turned out not to reach the strip
			// after all, which must not hold the runway for ever - before that, a clear
			// body is simply an agent that has not arrived yet.
			bClear = bBodyClear && (Agent.CrossingPhase == ECrossingPhase::OnStrip || !bFromIsBar);
		}
		else
		{
			// FALLBACK, for a plan whose polyline is too short to sample a body from: the
			// NODE rule this replaced. Off the strip, the node the step left ends it; on
			// it, the tail must be a full chain half width past that node. Kept rather
			// than deleted because a two-point plan is still a plan, and an agent on one
			// must not hold a runway for ever.
			double ChainHalfWidth = 0.0;
			const bool bNodeOnStrip = Network.IsGuidelineNodeOnRunway(FromId, CrossingChain, &ChainHalfWidth);
			const double TailPast = (T - F * 0.5) - UGroundTraffic::StepStart(Plan, Current);
			bClear = !bFromIsBar && TailPast >= 0.0 && (!bNodeOnStrip || TailPast > ChainHalfWidth);
		}

		if (bClear)
		{
			Agent.EndCrossing();

			// THE RELEASE LINE LIVES HERE, not at the Vacated handover where it started:
			// vacating no longer gives the runway back, it hands it to this rule, and a
			// line claiming otherwise would be a log that lies. Transition by
			// construction - the phase is None immediately after.
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Agent.Id);
		}
	}
}

void FClaimPass::BuildPending(const FRoadAgent& Agent, const URoadNetwork& Network,
	const FClaimWindow& Window, FPendingClaims& Pending) const
{
	const FRoutePlan& Plan = Agent.PlanInProgress();
	const double T = Window.T;
	const double F = Window.F;
	const double G = Window.G;
	// NOT const: step 0'' below extends it across a road-taxiway crossing's chain.
	double Head = Window.Head;
	const double Tail = Window.Tail;
	const int32 Current = Window.Current;

	Pending.Reserve(4);

	/**
	 * A RESERVED SURFACE CLAIM MUST NEVER OVERWRITE AN OCCUPIED ONE ALREADY WANTED THIS PASS.
	 *
	 * TryClaim treats a same-agent claim on the same resource as an UPDATE and writes it
	 * over the old one WHOLESALE, bOccupied included. A crossing raises its chain occupied
	 * at the front of Pending (route zero below), and two things later in route order raise
	 * the SAME chain as a reservation: a holding-position bar on the FAR side of the crossing -
	 * which is how a crossing is actually painted, one bar each side - and a runway edge on
	 * a step the agent has not reached yet. Either one downgraded the crossing's occupancy
	 * to a reservation, and a reservation is exactly what a landing may preempt: the table
	 * would then clear an aircraft onto an aeroplane standing on the centreline.
	 *
	 * Skipped rather than re-ordered or merged. Route order is what the first-refusal rule
	 * reads, so the entries cannot move; and there is nothing to ask for anyway - the agent
	 * already holds that surface, more strongly than the entry being dropped would hold it.
	 * Occupied entries are never skipped by this, so an occupancy raised later still lands.
	 */
	auto WantedOccupied = [&Pending](const FTrafficResource& Resource)
	{
		return Pending.ContainsByPredicate([&Resource](const FWantedClaim& Already)
		{
			return Already.Claim.bOccupied && Already.Claim.Resource == Resource;
		});
	};

	// 0. THE CLAIM SIDE OF THE CROSSING UpdateCrossing has just armed, advanced or
	// released. See Run for why the runway under a crossing agent is held at all.
	// AT THE FRONT of Pending, so a refusal here binds before anything further along the
	// route: the agent is standing on this surface, and nothing it might be told about a
	// node ahead can matter more than that. COMMITTED COUNTS AS STANDING ON IT: the body
	// is a moment from the asphalt and nothing may be cleared onto it in between.
	if (Agent.CrossingPhase != ECrossingPhase::None && Agent.CrossingRunway.IsSet())
	{
		// THROUGH Chains (issue #170), not a fresh Network.RunwayChainOrSeed walk: the same
		// seed UpdateCrossing just asked about above, so this is very likely already the
		// entry it just filled.
		const TArray<FRoadSegmentId>& Chain = Chains.GetOrSeed(Network, Agent.CrossingRunway);
		for (const FRoadSegmentId Segment : Chain)
		{
			FWantedClaim Crossing;
			Crossing.Claim.AgentId = Agent.Id;
			Crossing.Claim.Resource = FTrafficResource::OfSurface(Segment);

			// OCCUPIED, unlike the bar's reservation: the aeroplane is ON the strip now,
			// and spec §3.3 says nobody is evicted from ground they are standing on.
			Crossing.Claim.bOccupied = true;
			Crossing.Claim.Rank = TraversalPriority(Agent.Class);
			Crossing.Step = Current;
			Crossing.StepStart = UGroundTraffic::StepStart(Plan, Current);
			Crossing.StepEnd = Plan.Steps[Current].EndDistance;
			Pending.Add(Crossing);
		}
	}

	// COMMITTED TO A ROAD'S STOP LINE (taxiway strip stage 4; re-grounded after the final
	// review). A vehicle is committed once it can no longer be told to stop at the line:
	//   - its NOSE IS PAST IT, whatever its speed. A queue that stopped it there, or a rig
	//     crawling over it, leaves a body in the strip that no reservation may be taken from -
	//     the first cut read this off braking distance alone, which is zero at a standstill, and
	//     the claim fell back to a reservation an aircraft then took (review, critical 2);
	//   - or it is within braking distance of the line AND was not refused at it: it held the
	//     line's conflicts on its last pass, or was refused by nothing at all (a rebuild clears
	//     both the table and the arbitration, and a vehicle committed an instant before one was
	//     then stopped under the wing for 39 ticks). The first cut read "refused by nothing" ALONE,
	//     so a van following another - refused by its leader on most passes - never committed and
	//     crossed on a reservation (review, critical 1). HOLDING the conflicts is what counts.
	// Braking is off the window as WindowFor built it, before step 0'' extends it.
	// ENFORCED BY: Airside.Model.Traffic.VanStoppedAcrossTheLine (the nose rule), and
	// TruckYieldsAtDerivedCrossing's two-van convoy (the held rule) - both red without them.
	//
	// PAST BY MORE THAN StopLineTolerance (re-review, important 1): a vehicle HELD at the line is
	// parked with its nose exactly on it - RouteFollower stops at Travelled + StopWithin - so a
	// strict "nose > line" was decided by rounding, and a rebuild re-projecting the vehicle a hair
	// forward flipped a waiting van to committed, whose occupancy then beat the aircraft's
	// reservation. Five centimetres, the stop precision measured on these crossings (a van
	// creeping to a line stopped 1 uu past it); a real queue or rig overhang is metres.
	//
	// HELD, AND NOT REFUSED IN THE CHAIN (re-review, important 2): a conflict granted THIS pass
	// ahead of a refusal further along the chain (no room past the far strip edge) left the van
	// braking for the line while "holding" the conflict, and one tick inside braking distance
	// committed it into a strip it had no room to leave. LineStep is the step leaving the line;
	// a refusal there or beyond (the line's own refusal, or step 0'' holding it there) is BlockedStep >= LineStep.
	// ApplyClaims now also gives such grants back (bInChain), so the table agrees.
	constexpr double StopLineTolerance = 5.0;
	const double Braking = FMath::Max(0.0, Window.Head - T - F * 0.5 - G);
	auto LineCommitted = [&](const FGuidelineNode& Line, double LineAt, int32 LineStep)
	{
		if (T + F * 0.5 > LineAt + StopLineTolerance)
		{
			return true;
		}
		// AND NOT ALREADY AT IT: a vehicle the hold parked on the line reads a speed that has not
		// yet decayed to zero while its position is pinned there (measured: 50-76 uu/s at a
		// standstill), so its "braking distance" beat a distance of nothing, and a rebuild - which
		// clears WaitingOn - then committed it with an aircraft 3.6 m past the conflict.
		const double ToLine = LineAt - (T + F * 0.5);
		if (!(Braking > 0.0 && ToLine > StopLineTolerance && ToLine < Braking))
		{
			return false;
		}
		if (Agent.GetWaitingOn() == 0)
		{
			return true;
		}
		if (Agent.GetBlockedStep() >= LineStep)
		{
			return false;
		}
		bool bHeld = false;
		for (const FGuidelineNodeId Conflict : Line.ProtectsConflicts)
		{
			bHeld |= Table.FindClaim(Agent.Id, FTrafficResource::OfNode(Conflict)) != nullptr;
		}
		return bHeld;
	};

	// 0'. PAST A ROAD'S STOP LINE AT A TAXIWAY CROSSING: every conflict that line protects is
	// held OCCUPIED - and the room past the far strip edge reserved - until the TAIL is off the
	// far arm's end - which ComputeCrossingSetBacks put at
	// the far strip edge - for the runway crossing's reason above: the vehicle is in the strip,
	// and a reservation an aircraft's rank could take would leave it stopped under the wing. The
	// reservation step 2 raises at the line itself turns occupied at the same moment, once the
	// vehicle is committed; this is the same hold after the line is behind it.
	//
	// THE LINE IS FOUND ON THE GRAPH, walking back from the current step's edge through the
	// junction's vehicle pieces, NOT on the plan's earlier steps: a rebuild re-points only the
	// step an agent is on and those ahead (FPlanReResolver::ReResolvePlan), so the steps behind
	// hold dead handles, and a walk over them lost the line - and the hold - for a truck
	// rebuilt with its centre past the conflict (review, important 3). The current edge is
	// always live. A vehicle on no junction piece (AtJunction unset) costs one edge lookup.
	// ENFORCED BY: Airside.Model.Traffic.TruckYieldsAtDerivedCrossing, .TruckCrossesTwoTaxiwaysAtOneNode
	if (Agent.Class != ETraversalClass::Aircraft && Plan.Steps.IsValidIndex(Current))
	{
		// JUST PAST THE FAR ARM'S END the current step is the far lane, not a junction piece, and
		// its start IS the far end - with the tail still behind it, in the strip. The walk then
		// starts there and goes back through the junction's pieces arriving at it (re-review:
		// the hold used to drop the moment the CENTRE passed the far end, and the comment above
		// said "tail"). Only while the tail can still be behind that node, so a vehicle on any
		// other road pays nothing.
		const FGuidelineEdge* On = Network.GetGuidelineEdge(Plan.Steps[Current].Edge);
		const FGuidelineNodeId Start = On != nullptr ? (Plan.Steps[Current].bReversed ? On->B : On->A) : FGuidelineNodeId();
		const double CurrentStart = UGroundTraffic::StepStart(Plan, Current);
		const bool bPastFarEnd = On != nullptr && !On->AtJunction.IsSet() && T - CurrentStart < F * 0.5;
		const FGuidelineNode* Line = nullptr;
		if (On != nullptr && (On->AtJunction.IsSet() || bPastFarEnd))
		{
			TArray<FGuidelineNodeId, TInlineAllocator<8>> Frontier = { Start };
			TSet<FGuidelineNodeId> Seen = { Start };
			for (int32 Hop = 0; Hop < 32 && Line == nullptr && !Frontier.IsEmpty(); ++Hop)
			{
				const FGuidelineNodeId At = Frontier.Pop();
				const FGuidelineNode* Node = Network.GetGuidelineNode(At);
				if (Node == nullptr)
				{
					continue;
				}
				if (Node->HoldingPosition == EHoldingPositionKind::TaxiwayCrossing && !Node->ProtectsConflicts.IsEmpty())
				{
					Line = Node;
					break;
				}
				if (Node->Origin.IsSet() && !(At == Start && bPastFarEnd))
				{
					continue;
				}
				for (const FGuidelineEdgeId Id : Node->Incident)
				{
					const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id);
					if (Edge != nullptr && Edge->B == At && Edge->AtJunction.IsSet()
						&& (bPastFarEnd || Edge->AtJunction == On->AtJunction)
						&& Edge->AllowedTraffic.Allows(Agent.Class) && !Seen.Contains(Edge->A))
					{
						Seen.Add(Edge->A);
						Frontier.Add(Edge->A);
					}
				}
			}
		}
		if (Line != nullptr)
		{
			double FarEnd = bPastFarEnd ? CurrentStart : Plan.Steps.Last().EndDistance;
			for (int32 Ahead = Current; !bPastFarEnd && Ahead < Plan.Steps.Num(); ++Ahead)
			{
				const FGuidelineNode* To = Network.GetGuidelineNode(Plan.Steps[Ahead].To);
				if (To != nullptr && To->Origin.IsSet())
				{
					FarEnd = Plan.Steps[Ahead].EndDistance;
					break;
				}
			}
			// AND THE ROOM STAYS CLAIMED (re-review, important 3): the window keeps reaching a
			// footprint and a gap past the far arm's end until the tail is off it, as it did on the
			// approach. Past the last conflict no step ahead ends at one, so step 0'' finds no
			// chain, and the room's reservations were given back halfway across - for anything
			// merging onto the far arm to take, with the van's tail still in the strip.
			if (Tail < FarEnd)
			{
				Head = FMath::Max(Head, FarEnd + F + G);
			}
			if (Tail < FarEnd)
			{
				for (const FGuidelineNodeId Conflict : Line->ProtectsConflicts)
				{
					FWantedClaim Committed;
					Committed.Claim.AgentId = Agent.Id;
					Committed.Claim.Resource = FTrafficResource::OfNode(Conflict);
					Committed.Claim.bOccupied = true;
					Committed.Claim.Rank = RankAt(Network, Conflict, Agent.Class);
					Committed.Step = Current;
					Committed.StepStart = UGroundTraffic::StepStart(Plan, Current);
					Committed.StepEnd = Plan.Steps[Current].EndDistance;
					Pending.Add(Committed);
				}
			}
		}
	}

	// 0''. A CROSSING IS ONE BOX (final review, important 7, and the room check of important 4).
	// Splitting the lines at their conflict nodes cut one junction path into short pieces, and
	// the claim pass read each as a junction of its own: an aircraft's box-entry rule moved to
	// the FIRST piece, so one queued behind an aircraft on the far arm drove in and stopped
	// ACROSS THE ROAD, where on main the whole turn was the box and it waited a gap short of it;
	// and a vehicle could pass its stop line with nowhere to go beyond the far strip edge, stop
	// in the strip holding the conflicts, and hold an aircraft there for as long as its queue.
	//
	// So the CHAIN - the steps from the entry through every conflict node to the first node
	// that is not one - is claimed as the whole it replaces: the window reaches to the chain's
	// end (an aircraft: its end node, as the unsplit turn's box entry did) or, for a vehicle,
	// to a footprint and a gap PAST the far arm's end, so its tail can clear the strip; and a
	// refusal of anything in it, before the agent has entered, holds it at the entry - a
	// vehicle's nose on its stop line, an aircraft's centre a gap short of the chain, the box
	// rule's own point. A vehicle already committed is not held: it cannot stop at the line.
	// ENFORCED BY: Airside.Model.Traffic.AircraftQueueWaitsClearOfRoad, .VanQueueBetweenTwoTaxiways
	int32 ChainFirst = INDEX_NONE;
	double ChainHoldAt = -1.0;
	for (int32 Index = Current; Index < Plan.Steps.Num() && UGroundTraffic::StepStart(Plan, Index) < Head; ++Index)
	{
		const FGuidelineNode* To = Network.GetGuidelineNode(Plan.Steps[Index].To);
		if (To == nullptr || !To->bCrossingConflict)
		{
			continue;
		}
		ChainFirst = Index;
		const double ChainStart = UGroundTraffic::StepStart(Plan, Index);
		double ChainEnd = Plan.Steps.Last().EndDistance;
		for (int32 Ahead = Index + 1; Ahead < Plan.Steps.Num(); ++Ahead)
		{
			const FGuidelineNode* Next = Network.GetGuidelineNode(Plan.Steps[Ahead].To);
			if (Next == nullptr || !Next->bCrossingConflict)
			{
				ChainEnd = Plan.Steps[Ahead].EndDistance;
				break;
			}
		}
		const bool bVehicle = Agent.Class != ETraversalClass::Aircraft;
		if (Tail < ChainEnd)
		{
			Head = FMath::Max(Head, bVehicle ? ChainEnd + F + G : ChainEnd + G);
		}
		const FGuidelineNode* Entry = Network.GetGuidelineNode(UGroundTraffic::StepFromNode(Plan, Index));
		if (bVehicle && Entry != nullptr && Entry->HoldingPosition == EHoldingPositionKind::TaxiwayCrossing)
		{
			if (!LineCommitted(*Entry, ChainStart, Index))
			{
				ChainHoldAt = ChainStart - F * 0.5;
			}
		}
		else if (Entry != nullptr && !Entry->bCrossingConflict && T <= ChainStart)
		{
			ChainHoldAt = ChainStart - G;
		}
		break;
	}

	// 0'''. A RUNWAY CROSSING IS ONE BOX TOO: RELEASED ACROSS THE BAR ONLY WITH ITS EXIT CLEAR (PIE,
	// 2026-09-30). Two aircraft held short at the two bars of one crossing, opposite ways; the runway
	// freed, the first was granted its bar with nothing past the strip asked for, committed (0a), and
	// stopped on the centreline short of the far bar, where the second stood - waiting for the very
	// runway the first now held occupied. Each held what the other needed; the resolver replanned the
	// crosser for ever onto routes that all ran through the waiter, and the runway was shut with it.
	//
	// THE ROAD STOP LINE'S RULE (0'' above, the ONE BOX idiom), applied to a bar: from the step
	// leaving the bar to the EXIT - the first route node off the strip, which on a painted crossing
	// is the far bar - plus a footprint and a gap past it, so the tail can clear the strip, is one
	// all-or-nothing chain with the bar's own reservation. Refused anywhere in it before the agent
	// commits, ApplyClaims gives back every reservation granted in it this pass, the RUNWAY
	// included, and the agent holds with its nose on the bar. Once committed the window keeps
	// REACHING the room until the tail passes the exit, so the room is asked for on every pass and
	// stays held while it is granted - an aircraft arriving at the far side is then refused it and
	// waits short of it, on its own line. What that does NOT promise: the room is a RESERVATION, so
	// a higher rank (an authored priority at a node in it) can still take it, and a crosser refused
	// its room after committing stops wherever its stop point is, which may be on the asphalt.
	//
	// REJECTED: letting the committed crosser win and asking the waiter to back off. Nothing in this
	// model reverses on a taxiway, and the resolver's replan cannot free a node an aeroplane is
	// standing on. REJECTED: an explicit "is the far bar held by somebody waiting the other way"
	// test. The far side can be taken by a queue, a parked aircraft or a van as well as by an
	// opposite waiter, and the runway is lost the same way to each; the chain answers all of them.
	// A HEAD-ON ON ONE LINE IS STILL A JAM - each waiter stands on the other's exit - but it jams at
	// the bars with the runway free, which is the resolver's and the player's problem, not a closed
	// runway's. ENFORCED BY: Airside.Model.Traffic.CrossingExitClaimedBeforeRelease,
	// .CrossingHeadOnKeepsRunwayFree - both red without this block.
	//
	// NO EXIT, NO CHAIN: a departure's route ends ON the runway, and the line-up has no far side
	// to ask for. The ROOM STOPS AT THE NEXT BAR, short of its node's reach: a crossing between two
	// parallel runways must not ask for the second runway to cross the first - waiting at the
	// second bar is exactly what that bar was painted for.
	int32 ExitChainFirst = INDEX_NONE;
	double ExitHoldAt = -1.0;
	FGuidelineNodeId ExitBar;
	{
		const bool bCrossing = Agent.CrossingPhase != ECrossingPhase::None && Agent.CrossingRunway.IsSet();
		// USED ONLY INSIDE THIS BLOCK, with no other Chains call between: the cache's map may move
		// its entries when it grows.
		const TArray<FRoadSegmentId>* Strip = nullptr;
		int32 Onto = INDEX_NONE;
		if (bCrossing)
		{
			Strip = &Chains.GetOrSeed(Network, Agent.CrossingRunway);
			Onto = Current;
		}
		else
		{
			// THE FIRST ENTRY BAR THE WINDOW HAS REACHED - an exit bar (its next step leads away
			// from the strip) is passed over, which is how an arrival leaving the runway asks for
			// nothing here.
			for (int32 Index = Current; Index + 1 < Plan.Steps.Num() && Plan.Steps[Index].EndDistance < Head; ++Index)
			{
				const FGuidelineNode* Bar = Network.GetGuidelineNode(Plan.Steps[Index].To);
				if (Bar == nullptr || !Bar->HoldingPositionFor.IsSet())
				{
					continue;
				}
				const TArray<FRoadSegmentId>& BarChain = Chains.GetOrSeed(Network, Bar->HoldingPositionFor);
				if (StepLeadsOntoStrip(Network, Plan, Index + 1, BarChain))
				{
					Strip = &BarChain;
					Onto = Index + 1;
					ExitBar = Plan.Steps[Index].To;
					break;
				}
			}
		}

		double ExitAt = -1.0;
		int32 PastExit = INDEX_NONE;
		if (Strip != nullptr)
		{
			if (bCrossing && !StepLeadsOntoStrip(Network, Plan, Current, *Strip))
			{
				// ALREADY PAST THE EXIT NODE with the tail still on the strip: the step being driven
				// leads away, so the node it left is the exit.
				ExitAt = UGroundTraffic::StepStart(Plan, Current);
				PastExit = Current;
			}
			else
			{
				for (int32 Index = Onto; Index < Plan.Steps.Num(); ++Index)
				{
					if (!Network.IsGuidelineNodeOnRunway(Plan.Steps[Index].To, *Strip))
					{
						ExitAt = Plan.Steps[Index].EndDistance;
						PastExit = Index + 1;
						break;
					}
				}
			}
		}

		if (ExitAt >= 0.0)
		{
			double RoomTo = ExitAt + F + G;
			for (int32 Index = PastExit; Index < Plan.Steps.Num() && UGroundTraffic::StepStart(Plan, Index) < RoomTo; ++Index)
			{
				const FGuidelineNode* Next = Network.GetGuidelineNode(Plan.Steps[Index].To);
				if (Next != nullptr && Next->HoldingPositionFor.IsSet())
				{
					// CLIPPED ONLY WHERE THE BODY FITS between the exit and the next bar. Closer than
					// a footprint (review, 2026-09-30: parallel runways nearer than F + G), an aircraft
					// held at the second bar would have its tail on the first strip - so the room is
					// NOT clipped, the window reaches the second bar, and its runway reservation joins
					// this chain: both runways or neither, holding at the first bar.
					const double NextReach = ReachExcessAt(Rules, Reach, Network, Plan.Steps[Index].To, Plan.Steps[Index].Edge, Agent.Class);
					const double NextAt = Plan.Steps[Index].EndDistance - NextReach;
					if (NextAt - ExitAt >= F)
					{
						RoomTo = FMath::Min(RoomTo, NextAt);
					}
					break;
				}
			}
			if (Tail < ExitAt)
			{
				Head = FMath::Max(Head, RoomTo);
			}
			if (!bCrossing)
			{
				ExitChainFirst = Onto;
				ExitHoldAt = FMath::Max(0.0, UGroundTraffic::StepStart(Plan, Onto) - F * 0.5);
			}
		}
	}

	// THE ENTRY RULE FIRES FOR ONE BOX PER PASS - the first the window reaches that the agent
	// has not entered. See the loop below and Run's header for the trace that rejected
	// chaining it through every consecutive box.
	bool bBoxEntryTaken = false;

	// 1. THE NODE THE CURRENT STEP LEFT, while the centre is still within half a footprint
	// of it. Without this an agent that had just crossed a junction would release it with
	// its tail still inside, and the next claimant would drive into that tail.
	//
	// PLUS THE NODE'S REACH along this edge: where the edge leaves the node alongside
	// another - a stand's sweep arc hugging its taxiway - half a footprint releases the
	// node with the body still beside the other line. See NodeReach.h.
	const FGuidelineNodeId From = UGroundTraffic::StepFromNode(Plan, Current);
	if (T - UGroundTraffic::StepStart(Plan, Current)
		< F * 0.5 + ReachExcessAt(Rules, Reach, Network, From, Plan.Steps[Current].Edge, Agent.Class))
	{
		FWantedClaim Want;
		Want.Claim.AgentId = Agent.Id;
		Want.Claim.Resource = FTrafficResource::OfNode(From);
		Want.Claim.bOccupied = true;
		Want.Claim.Rank = RankAt(Network, From, Agent.Class);
		Want.Step = Current;
		Want.StepStart = UGroundTraffic::StepStart(Plan, Current);
		Want.StepEnd = Plan.Steps[Current].EndDistance;
		Pending.Add(Want);
	}

	// 1'. AN AIRCRAFT KEEPS A CONFLICT UNTIL ITS WING HAS CLEARED IT - half its span past, not
	// half its footprint (re-review, measured on Airside.Model.Traffic.VanHeldAtLineThroughRebuilds).
	// The strip, and so the stop line, is sized from the wing, and the rule the crossing is judged
	// by is the plan's: no vehicle into the strip while the aircraft is within its half-span of
	// the crossing. Released at half a footprint (5 m) a 2 m/s aircraft handed the conflict back
	// 0.8 s early - the Piper's half-span is 6.6 m - and the van waiting at the line started in.
	// Every conflict behind it on its route, not only the one the current step left: the pieces
	// between two lanes' conflicts are 3 m, shorter than the span.
	//
	// FOUND ON THE LIVE GRAPH NEAR THE NODE THE CURRENT STEP LEFT, by where it lies from the
	// body - behind it on its track, within Clear - not by the plan's steps behind it: a rebuild
	// re-points only the step the agent is on and those ahead, and a rebuild landing just after
	// the aircraft passed a conflict handed it straight to the van waiting at the line (measured
	// on VanHeldAtLineThroughRebuilds: the van held C, occupied, 3.6 m behind the wing).
	if (Agent.Class == ETraversalClass::Aircraft && Plan.Steps.IsValidIndex(Current))
	{
		const double Clear = FMath::Max(F * 0.5, Agent.Wingspan() * 0.5);
		const FVector2D Here = Agent.LastMotion.Position;
		const FVector2D Ahead(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading));
		auto IsBehindWithinClear = [&](const FVector2D& At)
		{
			const double Back = FVector2D::DotProduct(Here - At, Ahead);
			return Back > 0.0 && Back < Clear && FMath::Abs(FVector2D::CrossProduct(Ahead, At - Here)) < 200.0;
		};
		// SEEDED FROM THE STEP'S END AS WELL AS ITS START: the from-node can itself be a dead
		// handle for the tick or two after a rebuild (measured: the agent's centre, which this pass
		// steps by, was one step past the step the re-resolve re-pointed from), and the end is not.
		TArray<FGuidelineNodeId, TInlineAllocator<8>> Near = { UGroundTraffic::StepFromNode(Plan, Current), Plan.Steps[Current].To };
		for (int32 Hop = 0; Hop < 3; ++Hop)
		{
			const int32 Known = Near.Num();
			for (int32 K = 0; K < Known; ++K)
			{
				const FGuidelineNode* Node = Network.GetGuidelineNode(Near[K]);
				for (const FGuidelineEdgeId Id : Node != nullptr ? Node->Incident : TArray<FGuidelineEdgeId>())
				{
					if (const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id))
					{
						Near.AddUnique(Edge->A == Near[K] ? Edge->B : Edge->A);
					}
				}
			}
		}
		for (const FGuidelineNodeId Candidate : Near)
		{
			const FGuidelineNode* Behind = Network.GetGuidelineNode(Candidate);
			const FTrafficResource Resource = FTrafficResource::OfNode(Candidate);
			if (Behind == nullptr || !Behind->bCrossingConflict || WantedOccupied(Resource)
				|| !IsBehindWithinClear(Behind->Position))
			{
				continue;
			}
			FWantedClaim Wing;
			Wing.Claim.AgentId = Agent.Id;
			Wing.Claim.Resource = Resource;
			Wing.Claim.bOccupied = true;
			Wing.Claim.Rank = RankAt(Network, Candidate, Agent.Class);
			Wing.Step = Current;
			Wing.StepStart = UGroundTraffic::StepStart(Plan, Current);
			Wing.StepEnd = Plan.Steps[Current].EndDistance;
			Pending.Add(Wing);
		}
	}

	// Where step 2's forward claims begin in Pending, so step 0'' can write its hold onto them.
	const int32 FirstForward = Pending.Num();

	// 2. Forward along the steps while the window still has distance left.
	for (int32 Index = Current; Index < Plan.Steps.Num(); ++Index)
	{
		const FRouteStep& Step = Plan.Steps[Index];
		const double Start = UGroundTraffic::StepStart(Plan, Index);
		if (Start >= Head)
		{
			break;
		}
		const double End = Step.EndDistance;
		const double Length = End - Start;

		// Where this edge has actually parted from the others at its end node, measured
		// back from the node - the node's claim begins there, not at the node. 0 at an
		// ordinary junction. See NodeReach.h.
		const double ExcessTo = ReachExcessAt(Rules, Reach, Network, Step.To, Step.Edge, Agent.Class);

		// A BOX: an edge too short to stand on without still blocking the node behind it,
		// which is what every junction turn path is. Spec §3.1.
		const bool bBox = Length < F + G;

		// ENTRY ONLY, AND THE FIRST BOX ONLY.
		//
		// Entry, because once the agent is INSIDE the box (T past its start) a refused end
		// node stops it short of that node like any other. The FIRST, because the window
		// routinely spans several boxes - three 400 uu junction paths sit inside a taxiing
		// van's window - and demanding the far end of every one of them is spec §3.1's
		// rejected alternative: the agent then refuses to move until a node two junctions
		// ahead is free, and the agent holding that node is waiting on it. Measured on
		// Airside.Model.Traffic.BoxEntryFirstOnly: chaining offered a stop point 400 uu short
		// of where the held node actually was, keeping the van out of an empty box.
		const bool bBoxEntry = bBox && T <= Start && !bBoxEntryTaken;
		bBoxEntryTaken = bBoxEntryTaken || bBoxEntry;

		const double Lo = FMath::Max(Tail, Start);
		const double Hi = FMath::Min(Head, End);
		if (Hi > Lo)
		{
			// Route distance to EDGE distance, and the mirror a reversed step needs, in
			// FClaimGeometry::EdgeInterval - which says why it is that way round and is
			// pinned by Airside.Model.Traffic.ClaimGeometry.
			const FClaimGeometry::FEdgeInterval Interval =
				FClaimGeometry::EdgeInterval(Lo, Hi, Start, Length, Step.bReversed);

			FWantedClaim Want;
			Want.Claim.AgentId = Agent.Id;
			Want.Claim.Resource = FTrafficResource::OfEdge(Step.Edge);
			Want.Claim.From = Interval.From;
			Want.Claim.To = Interval.To;

			// OCCUPIED on the step the agent is standing on - never preemptable, because
			// nobody may be evicted from ground they are on. Everything beyond is a
			// reservation a higher rank may take.
			Want.Claim.bOccupied = (Index == Current);

			// The node an edge leads INTO governs it: an authored "vehicles first" at a
			// junction has to reach the approach, not just the junction itself. The step
			// being stood on is ranked at the node it left, which is the one it is in.
			Want.Claim.Rank = Index == Current
				? RankAt(Network, UGroundTraffic::StepFromNode(Plan, Current), Agent.Class)
				: RankAt(Network, Step.To, Agent.Class);

			Want.Step = Index;
			Want.StepStart = Start;
			Want.StepEnd = End;
			Want.EdgeLength = Length;
			Want.bReversed = Step.bReversed;
			Pending.Add(Want);

			// THE RUNWAY UNDER THE EDGE - spec §3.1's first route to the surface rule.
			// Raised HERE, immediately after the edge it belongs to, so Pending stays in
			// ROUTE ORDER: the first refusal in that order is what decides where the agent
			// stops, and a surface entry filed out of order would offer a stop point for
			// line the agent reaches later than one it reaches sooner.
			//
			// THE WHOLE CHAIN, not the one segment this guideline was derived from: a runway
			// is several segments once it has exits, and holding only the piece under this
			// edge would let a second aircraft onto the same strip further down. The same
			// reason DispatchArrival and ArmDepartureIfRunway both record chains.
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			if (Edge != nullptr && Edge->DerivedFrom.IsSet() && Network.IsRunwaySegment(Edge->DerivedFrom))
			{
				// THROUGH Chains (issue #170): a long runway derives several short guideline
				// steps, one Edge->DerivedFrom apiece but very often the SAME chain, and this
				// loop used to re-walk it once per step this window touches.
				for (const FRoadSegmentId Segment : Chains.Get(Network, Edge->DerivedFrom))
				{
					// COPIED FROM THE EDGE CLAIM, rank included: the surface is claimed
					// BECAUSE of that edge, and ranking the two differently would let an
					// agent win the runway and lose the line onto it, or the reverse.
					FWantedClaim OnRunway = Want;
					OnRunway.Claim.Resource = FTrafficResource::OfSurface(Segment);

					// A surface is held whole, so the interval fields mean nothing on it -
					// cleared rather than left carrying the edge's, where a later reader
					// would take them for a real extent.
					OnRunway.Claim.From = 0.0;
					OnRunway.Claim.To = 0.0;

					// OCCUPIED ONLY ON THE STEP THE AGENT IS STANDING ON, the same rule the
					// edge claim above uses and for the same reason: an aircraft actually on
					// the strip may not be evicted from it, while one merely approaching
					// holds a reservation a landing's occupancy can take.
					OnRunway.Claim.bOccupied = (Index == Current);
					OnRunway.Surface = FWantedClaim::ESurface::RunwayEdge;

					// See WantedOccupied: a reservation on a chain this agent is already
					// standing on would be written over its own occupancy.
					if (!OnRunway.Claim.bOccupied && WantedOccupied(OnRunway.Claim.Resource))
					{
						continue;
					}
					Pending.Add(OnRunway);
				}
			}
		}

		// THE NODE IS ASKED FOR when the window reaches its REACH, not the node itself:
		// the stop point a refusal offers is that much further back, and a window that only
		// reached the node would learn of the refusal with less than a braking distance left.
		if (End - ExcessTo < Head || bBoxEntry)
		{
			FWantedClaim Want;
			Want.Claim.AgentId = Agent.Id;
			Want.Claim.Resource = FTrafficResource::OfNode(Step.To);

			// Occupied only while the CENTRE is within half a footprint of the node, which
			// is the same test the left-behind node above uses, read forwards - and the
			// same reach added, for the same reason.
			Want.Claim.bOccupied = FMath::Abs(End - T) < F * 0.5 + ExcessTo;
			Want.Claim.Rank = RankAt(Network, Step.To, Agent.Class);
			Want.Step = Index;
			Want.bEndNode = true;
			Want.bBoxEntry = bBoxEntry;
			Want.ReachExcess = ExcessTo;
			Want.StepStart = Start;
			Want.StepEnd = End;
			Want.EdgeLength = Length;
			Want.bReversed = Step.bReversed;
			// A CONFLICT NODE ALREADY WANTED OCCUPIED - a committed vehicle's hold on the crossing
			// it is in (steps 0' and the stop line) - is not asked for again as a reservation: the
			// table would write the reservation over the occupancy (see WantedOccupied) and hand
			// the conflict to the next aircraft to ask. Measured: once step 0'' stretched a
			// vehicle's window to the far strip edge, every committed truck downgraded its own
			// hold this way, and at 3-4 s behind it the aircraft drove into the crossing with the
			// truck still on it.
			if (Want.Claim.bOccupied || !WantedOccupied(Want.Claim.Resource))
			{
				Pending.Add(Want);
			}

			// A NODE CARRYING A BAR CLAIMS THE RUNWAY IT PROTECTS - spec §3.1's second
			// route to the surface rule - so the stop happens AT THE BAR rather than at the
			// runway edge. On a taxiway that crosses a runway the crossing guideline is
			// derived from the TAXIWAY, so the rule above sees nothing and the bar is the
			// only thing between the agent and the strip.
			//
			// INSIDE the node-claim block, and so only once the window has reached the bar.
			// The alternative - claiming it for every step this loop visits - reserves the
			// strip from the far side of the airport, and ArrivalPlanner refuses a landing on
			// ANY held claim, reserved or not: an aircraft merely ROUTED past a bar would
			// close the runway for the whole of its taxi. Once End < Head the agent is within
			// braking distance plus a gap of the bar, which is when it needs the answer.
			//
			// APPLIES TO EVERY CLASS. A van crossing a live runway is the case a bar is for;
			// nothing here reads Agent.Class.
			const FGuidelineNode* Node = Network.GetGuidelineNode(Step.To);
			if (Node != nullptr && Node->HoldingPositionFor.IsSet())
			{
				// The chain, expanded HERE rather than stored at the bar, so an exit added to
				// a runway after the bar was placed still protects the whole strip - see
				// URoadNetwork::RunwayChain. Empty means the segment is no longer a runway
				// (the profile changed under the mark) and the named segment alone is still
				// honoured: a bar that silently stopped protecting anything is worse than one
				// protecting a piece of what it used to. THROUGH Chains (issue #170): a bar
				// visited on several ticks, or by several agents in one Arbitrate pass, asks
				// for the same seed every time.
				const TArray<FRoadSegmentId>& Chain = Chains.GetOrSeed(Network, Node->HoldingPositionFor);

				// ONLY A BAR THIS ROUTE CROSSES AT (2026-09-30): the step leaving it must lead onto the
				// strip. A route that reaches a bar node and turns AWAY - along a taxiway joining there,
				// or off the far bar of a crossing it has already made - is not going onto the runway,
				// and reserving it held the agent at the bar for a strip it never meant to enter. That
				// is how a deadlock replan that sent a waiter the other way round (F1 -> F2 in
				// Airside.Model.Traffic.CrossingHeadOnReplansOffTheCycle) was refused at the very bar
				// it was turning away from, re-formed the cycle, and was replanned back, every window.
				// A route that ENDS at a bar crosses nothing either. The crossing's own occupancy
				// (step 0) is untouched: it is the body, not the bar.
				// ENFORCED BY: Airside.Model.Traffic.CrossingHeadOnReplansOffTheCycle, .AsymmetricBarToBarHolds.
				const bool bEntersStrip = StepLeadsOntoStrip(Network, Plan, Index + 1, Chain);
				for (const FRoadSegmentId Segment : Chain)
				{
					if (!bEntersStrip)
					{
						break;
					}
					FWantedClaim Bar;
					Bar.Claim.AgentId = Agent.Id;
					Bar.Claim.Resource = FTrafficResource::OfSurface(Segment);

					// RESERVED, NEVER OCCUPIED - nobody is ever occupied THROUGH a bar. An
					// agent still short of one is by definition not on the runway, and an
					// occupied claim cannot be preempted, so a queue at the bar would lock
					// the strip against the very landing the bar exists to protect.
					Bar.Claim.bOccupied = false;
					Bar.Claim.Rank = RankAt(Network, Step.To, Agent.Class);
					Bar.Step = Index;
					Bar.StepStart = Start;
					Bar.StepEnd = End;
					Bar.EdgeLength = Length;
					Bar.bReversed = Step.bReversed;
					Bar.Surface = FWantedClaim::ESurface::HoldingPosition;
					Bar.HoldNode = Step.To;

					// THE FAR BAR OF A CROSSING, and the reason WantedOccupied exists: the
					// agent is already ON this strip, holding it occupied, so there is
					// nothing for a bar to protect it from and everything for the bar's
					// reservation to spoil. Airside.Model.Traffic.CrossingHoldsRunway
					// measures it with a bar on each side of the runway. SINCE bEntersStrip
					// (2026-09-30) that far bar raises nothing - its next step leads away - so
					// this skip now guards a route that re-enters the strip it is still
					// crossing; kept because the downgrade it prevents is the same.
					if (WantedOccupied(Bar.Claim.Resource))
					{
						continue;
					}
					Pending.Add(Bar);
				}
			}

			// A ROAD'S STOP LINE AT A TAXIWAY CROSSING RESERVES ITS CONFLICT NODES - the runway
			// bar's pattern applied to a node (taxiway strip stage 4): the vehicle asks for every
			// node where its lane crosses an aircraft line BEFORE it passes the line, so a refusal
			// stops it with its nose on the line, at the strip edge and clear of a passing wing,
			// rather than a gap short of the conflict itself, inside the strip. Without this the
			// conflict's own node claim still decides who goes, but the loser waits under the wing.
			//
			// RESERVED for the bar's own reason: a queue at the line must not lock the conflict
			// against the aircraft it protects. Aircraft need no change - their route runs THROUGH
			// the conflict node, so their ordinary node claims contend with this reservation, and
			// rank decides.
			//
			// UNTIL THE VEHICLE IS COMMITTED (LineCommitted, above): then OCCUPIED, and it stays so
			// across the strip (step 0' of this pass). A reservation a higher rank may take is
			// right for a vehicle that can still stop, and wrong for one that cannot - measured
			// before this rule on the arrival sweep in TruckYieldsAtDerivedCrossing: an aircraft
			// arriving 1-4 s after the truck took the conflict from a truck already committed, and
			// the truck stopped inside the strip, under the wing, for 40 ticks. A vehicle REFUSED
			// at the line is not committed by braking distance: turning its ask into an occupancy -
			// which presence lets beat any reservation - took the conflict from the aircraft that
			// held it (measured: the aircraft 4 s ahead then waited 8 s for the truck).
			// ENFORCED BY: Airside.Model.Traffic.TruckYieldsAtDerivedCrossing
			if (Node != nullptr && Node->HoldingPosition == EHoldingPositionKind::TaxiwayCrossing)
			{
				const bool bCommitted = LineCommitted(*Node, End, Index + 1);
				for (const FGuidelineNodeId Conflict : Node->ProtectsConflicts)
				{
					FWantedClaim Line;
					Line.Claim.AgentId = Agent.Id;
					Line.Claim.Resource = FTrafficResource::OfNode(Conflict);
					Line.Claim.bOccupied = bCommitted;
					// Ranked AT THE CONFLICT, as every other claim on that node is: an authored
					// priority there has to govern the reservation as well as the crossing.
					Line.Claim.Rank = RankAt(Network, Conflict, Agent.Class);
					Line.Step = Index;
					Line.StepStart = Start;
					Line.StepEnd = End;
					Line.EdgeLength = Length;
					Line.bReversed = Step.bReversed;
					Line.Surface = FWantedClaim::ESurface::HoldingPosition;
					Line.HoldNode = Step.To;
					if (WantedOccupied(Line.Claim.Resource))
					{
						continue;
					}
					Pending.Add(Line);
				}
			}
		}
	}

	// STEP 0'' HOLD, onto every forward claim raised inside the chain.
	if (ChainHoldAt >= 0.0)
	{
		for (int32 Index = FirstForward; Index < Pending.Num(); ++Index)
		{
			if (Pending[Index].Step >= ChainFirst)
			{
				Pending[Index].HoldAt = ChainHoldAt;
				Pending[Index].HoldStep = ChainFirst;
			}
			// The approach step's claims too - a vehicle's stop-line reservations are raised there.
			Pending[Index].bInChain = Pending[Index].Step >= ChainFirst - 1;
		}
	}

	// STEP 0''' HOLD, the same way: the claims from the step leaving the bar on hold the agent with
	// its nose on the bar, and the bar step's own - the runway's reservation among them - are in the
	// chain, so a refused exit gives the runway back. The nearer hold wins where both chains apply.
	if (ExitChainFirst != INDEX_NONE)
	{
		for (int32 Index = FirstForward; Index < Pending.Num(); ++Index)
		{
			FWantedClaim& Want = Pending[Index];
			Want.bInExitChain = Want.Step >= ExitChainFirst - 1;
			if (Want.Step >= ExitChainFirst && (Want.HoldAt < 0.0 || ExitHoldAt < Want.HoldAt))
			{
				Want.HoldAt = ExitHoldAt;
				Want.HoldStep = ExitChainFirst;
				Want.CrossingBar = ExitBar;
			}
		}
	}
}

void FClaimPass::ApplyClaims(FRoadAgent& Agent, const FClaimWindow& Window,
	const FPendingClaims& Pending)
{
	// WHAT WAS ACTUALLY CLAIMED, not what was wanted: the loop below stops reserving at the
	// first refusal, so a resource further along the route was never asked for this pass and
	// keeping the agent's stale hold on it would block everybody else for line the agent has
	// just been told it cannot reach. Ground the agent is STANDING on is kept whether or not
	// the claim was granted - it is standing there either way.
	//
	// Released AFTER the claims rather than before them, which is safe and was checked: the
	// table skips an agent's own claims when it looks for conflicts, and no other agent
	// claims between this ReleaseExcept and the ones above - Arbitrate runs one agent at a
	// time. Releasing everything and re-claiming is still rejected (Run's header):
	// this drops only what was not asked for.
	//
	// MEMBER, NOT A LOCAL (issue #168): reset here rather than declared fresh, so its
	// capacity survives from one agent's pass to the next instead of a malloc/free every
	// time. See the header for why Pending does not get the same treatment.
	Wanted.Reset();
	Wanted.Reserve(Pending.Num());

	const int32 WasWaitingOn = Agent.GetWaitingOn();
	const int32 WasBlockedStep = Agent.GetBlockedStep();
	bool bHeld = false;

	// Everyone this pass overlapped, for the Warning's throttle. See FRoadAgent::LastOverlaps.
	// MEMBER too, reset per agent - same reasoning as Wanted just above.
	OverlapsThisPass.Reset();

	// ONE BOX IS ALL OR NOTHING (re-review, important 4): the reservations granted inside a
	// crossing chain this pass, given back the moment anything in the chain is refused. An
	// aircraft queued a gap short of a crossing, behind one standing on the far arm, held both
	// road lanes' conflicts - granted ahead of its refusal in route order - for as long as the
	// queue lasted, with the crossing empty; a van the queue's own stand needed could never
	// cross. Given back, the lanes are free between this agent's passes.
	// ENFORCED BY: Airside.Model.Traffic.VanCrossesPastAircraftQueue
	//
	// ONE LIST PER BOX (review, 2026-09-30): the runway exit chain (bInExitChain) is a second box
	// that can share a window with the road one, and a refusal in either gives back only its own.
	// ENFORCED BY: no test yet - staging an aircraft inside a derived road-taxiway crossing with a
	// runway bar inside its window needs a solved junction beside a runway; the split is kept
	// small enough to read instead.
	TArray<FTrafficResource, TInlineAllocator<8>> ChainGranted;
	bool bChainRefused = false;
	TArray<FTrafficResource, TInlineAllocator<8>> ExitChainGranted;
	bool bExitChainRefused = false;
	auto GiveBack = [this](TArrayView<const FTrafficResource> Granted)
	{
		for (const FTrafficResource& Given : Granted)
		{
			Wanted.RemoveSingle(Given);
		}
	};

	for (const FWantedClaim& Want : Pending)
	{
		// PAST THE FIRST REFUSAL, only ground the agent occupies is still claimed. Skipping
		// its own occupancies here was a defect: an agent refused a node it was STANDING on
		// abandoned the rest of its own body, and the agent that had merely reserved that
		// node drove into it.
		if (bHeld && !Want.Claim.bOccupied)
		{
			// A CROSSING SOMEBODY ELSE IS IN STILL HOLDS THIS AGENT AT ITS ENTRY, even when the
			// first refusal is short of it (re-review: a van following another over a crossing,
			// refused by its leader, was never told the crossing beyond was taken; when the leader
			// cleared the line the follower was inside its braking distance of it and stopped with
			// its nose 35 cm in the strip for 10 s). Not claimed - reservations past a refusal
			// never are - only looked at, and only a node another agent holds.
			if ((Want.bInChain || Want.bInExitChain) && Want.Claim.Resource.Kind == ETrafficResourceKind::Node
				&& Table.IsHeld(Want.Claim.Resource, Agent.Id))
			{
				const double Cap = Want.Surface == FWantedClaim::ESurface::HoldingPosition
					? FMath::Max(0.0, Want.StepEnd - Window.T - Window.F * 0.5)
					: (Want.HoldAt >= 0.0 ? FMath::Max(0.0, Want.HoldAt - Window.T) : -1.0);
				if (Cap >= 0.0 && Cap < Agent.GetStopWithin())
				{
					Agent.Refuse(Agent.GetBlockedStep(), Agent.GetBlockedResource(), Cap, Agent.GetWaitingOn());
				}
			}
			continue;
		}

		FTrafficClaim Blocker;
		const bool bGranted = Table.TryClaim(Want.Claim, Blocker) == EClaimResult::Granted;
		if (bGranted || Want.Claim.bOccupied)
		{
			Wanted.Add(Want.Claim.Resource);
		}
		if (bGranted && Want.bInChain && !Want.Claim.bOccupied)
		{
			ChainGranted.Add(Want.Claim.Resource);
		}
		if (bGranted && Want.bInExitChain && !Want.Claim.bOccupied)
		{
			ExitChainGranted.Add(Want.Claim.Resource);
		}
		if (!bGranted && Want.bInChain && !bChainRefused)
		{
			bChainRefused = true;
			GiveBack(ChainGranted);
		}
		if (!bGranted && Want.bInExitChain && !bExitChainRefused)
		{
			bExitChainRefused = true;
			GiveBack(ExitChainGranted);
		}
		if (bGranted)
		{
			continue;
		}

		// AN OCCUPIED CLAIM CAN ONLY BE REFUSED BY ANOTHER OCCUPANT (see TryClaim), and on a
		// NODE or a SURFACE that means two bodies in one place - worth saying out loud, once
		// per new blocker. On an EDGE it does not: one claim per agent per edge means the
		// interval carries the agent's whole window as well as its body, so two of them
		// overlapping is the ordinary head-on and queueing case, and warning about it would
		// fire on every stopped queue on the airport.
		if (Want.Claim.bOccupied && Want.Claim.Resource.Kind != ETrafficResourceKind::Edge)
		{
			// THROTTLED ON THE OVERLAP'S OWN HISTORY, not on WaitingOn. WaitingOn names
			// the FIRST refusal in route order, so an overlap that was not the first
			// refusal never matched it and this Warning fired on every single tick for as
			// long as the overlap lasted - which is how a log stops being read at all.
			if (!Agent.GetLastOverlaps().Contains(Blocker.AgentId))
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d overlaps agent %d on %s: both are standing on it"),
					Agent.Id, Blocker.AgentId, *Want.Claim.Resource.Describe());
			}
			OverlapsThisPass.AddUnique(Blocker.AgentId);
		}

		if (bHeld)
		{
			// THE FIRST REFUSAL IN ROUTE ORDER DECIDES, which is not always the NEAREST
			// stop point - that claim was true until surfaces arrived and is not any
			// more. A step now contributes an edge entry AND a surface entry, in that
			// order, and the edge's refusal stops the agent at the blocker's boundary
			// INSIDE the step while the surface's would have stopped it a gap short of
			// the step's start, which is nearer.
			//
			// ACCEPTED, and not re-ordered to take the minimum: the edge refusal means an
			// aircraft is already on that line, so the agent queues up behind it on the
			// strip rather than waiting off it. That is worse for the runway and better
			// for the queue, and it is what the same rule already does at every other
			// shared edge on the airport - a crossing agent holds the surface OCCUPIED
			// (route four above), so the aircraft it queued behind is one the table
			// already knows about, not a landing that could be cleared onto it.
			continue;
		}

		bHeld = true;
		int32 NewBlockedStep = Want.Step;

		// A BAR-HOLDER'S BLOCKED STEP IS THE ONE LEAVING THE BAR, not the one arriving at
		// it. The refusal was raised for the step that ENDS at the bar node, and the agent
		// stops with its nose on the bar - which is AT the start of the next step. The
		// deadlock resolver asks "is this agent at the node its refused step leaves from",
		// and measured against the arriving step a departure waiting at a bar was never a
		// candidate: the one member of a bar-versus-arrival cycle who could have turned was
		// never asked (PIE, 2026-09-06). Only when there IS a next step; a bar at the end of
		// a route is the route's end.
		if (Want.Surface == FWantedClaim::ESurface::HoldingPosition
			&& Agent.PlanInProgress().Steps.IsValidIndex(Want.Step + 1))
		{
			NewBlockedStep = Want.Step + 1;
		}
		// The transition tests below compare against what was STORED last pass, so they
		// read the stored value, not Want.Step - a bar refusal stores the next step, and
		// comparing the raw one logged "holding short" on every tick.

		double NewStopWithin = StopWithinFor(Want, Blocker, Window);

		// A CROSSING'S ENTRY HOLD (BuildPending's step 0''): refused inside the chain before
		// entering it, the agent waits at the entry, not wherever this claim alone would stop it.
		//
		// A RUNWAY EXIT'S HOLD TAKES A TIE as well (bExitHold): an agent already stopped on its bar
		// reads 0 both ways, and it is still held AT THE BAR - its blocked step must be the one
		// leaving it, which is where the deadlock resolver looks for a waiter that can turn.
		bool bExitHold = false;
		if (Want.HoldAt >= 0.0)
		{
			const double Hold = FMath::Max(0.0, Want.HoldAt - Window.T);
			if (Hold < NewStopWithin || (Want.CrossingBar.IsSet() && Hold <= NewStopWithin))
			{
				NewStopWithin = Hold;
				NewBlockedStep = Want.HoldStep;
				bExitHold = Want.CrossingBar.IsSet();
			}
		}

		// ON THE TRANSITION ONLY. Logged every tick this would be one line per agent per
		// frame, which is how a log stops being read at all.
		//
		// A BAR GETS ITS OWN LINE INSTEAD OF THE GENERIC ONE - spec §10 lists "holding-position
		// reached" separately from "stopped for a resource" - because it names the node
		// and the segment as well as the holder, and one event must not produce two
		// lines. Its transition test is the blocker OR the step: an agent already waiting
		// on the same holder for something else is still newly held HERE, and BlockedStep
		// is what changed when it became so.
		//
		// A BAR HELD FOR ITS EXIT gets its own line for the same reason: "stops N uu short of node F"
		// would name the far side and hide that the agent is waiting at a bar with the runway free.
		if (bExitHold)
		{
			if (WasWaitingOn != Blocker.AgentId || WasBlockedStep != NewBlockedStep)
			{
				UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d holding short at node %d: crossing exit %s held by agent %d"),
					Agent.Id, Want.CrossingBar.Index, *Want.Claim.Resource.Describe(), Blocker.AgentId);
			}
		}
		else if (Want.Surface == FWantedClaim::ESurface::HoldingPosition)
		{
			if (WasWaitingOn != Blocker.AgentId || WasBlockedStep != NewBlockedStep)
			{
				// A ROAD STOP LINE reserves a conflict NODE, not a runway surface - named as what it is.
				if (Want.Claim.Resource.Kind == ETrafficResourceKind::Node)
				{
					UE_LOG(LogAirsideTraffic, Log,
						TEXT("Agent %d waiting at the stop line at node %d for taxiway crossing %s held by agent %d"),
						Agent.Id, Want.HoldNode.Index, *Want.Claim.Resource.Describe(), Blocker.AgentId);
				}
				else
				{
					UE_LOG(LogAirsideTraffic, Log,
						TEXT("Agent %d holding short at node %d for runway segment %d held by agent %d"),
						Agent.Id, Want.HoldNode.Index, Want.Claim.Resource.Surface.Index, Blocker.AgentId);
				}
			}
		}
		else if (WasWaitingOn != Blocker.AgentId)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d stops %.0f uu short of %s held by agent %d"),
				Agent.Id, NewStopWithin, *Blocker.Resource.Describe(), Blocker.AgentId);
		}

		// ONE CALL, FOUR FIELDS (issue #174) - BlockedStep, BlockedResource, StopWithin and
		// WaitingOn used to be four separate assignments here, which is exactly the shape
		// that let GroundTrafficRebuild.cpp hand-type a partial reset of the same fields
		// instead of calling ClearArbitration. See FRoadAgent::Refuse.
		Agent.Refuse(NewBlockedStep, Want.Claim.Resource, NewStopWithin, Blocker.AgentId);

		// NO break: the loop above skips every RESERVATION past this point (claiming line
		// beyond a refusal would hold it against everybody for a journey the agent is not
		// making this tick) but must go on claiming the ground the agent is standing on.
	}

	Agent.SetLastOverlaps(MoveTemp(OverlapsThisPass));
	Table.ReleaseExcept(Agent.Id, Wanted);

	if (!bHeld)
	{
		// LastOverlaps is NOT reset here: it was already assigned this pass's real value just
		// above, and ClearArbitration deliberately leaves it alone - see the declaration.
		Agent.ClearArbitration();
		if (WasWaitingOn != 0)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d resumes"), Agent.Id);
		}
	}
}

double FClaimPass::StopWithinFor(const FWantedClaim& Want, const FTrafficClaim& Blocker,
	const FClaimWindow& Window)
{
	const double T = Window.T;
	const double F = Window.F;
	const double G = Window.G;

	if (Want.Surface == FWantedClaim::ESurface::RunwayEdge)
	{
		// A GAP SHORT OF WHERE THE RUNWAY BEGINS - the step's START, not its end.
		// The refused thing is the strip this edge lies on, so stopping short of the
		// edge's far end would leave the agent standing on the runway it was just
		// refused. On the step it is already standing on this is 0, which is the only
		// honest answer: it cannot stop short of where it already is.
		return FMath::Max(0.0, Want.StepStart - T - G);
	}

	if (Want.Surface == FWantedClaim::ESurface::HoldingPosition)
	{
		// THE NOSE STOPS ON THE BAR, which is why this is the one refusal that does
		// not subtract the gap: a bar is the position an aircraft is required to hold
		// AT, and stopping G short would leave it short of the mark the player
		// painted. Travelled is the CENTRE, so the nose is at T + F/2.
		//
		// A BAR AT THE FAR END OF A BOX STEP THEREFORE BEATS THE BOX-ENTRY RULE,
		// which would have stopped the agent a gap short of the box's START. That is
		// deliberate and not an oversight: the box rule guesses where an agent can
		// safely wait, and the bar is where the player SAID it waits. Both entries
		// are in Pending and the bar's is second, so this only differs from the box
		// answer when the box's end node was granted and the surface was not - i.e.
		// when the strip is busy but the junction is not, which is the case the
		// player painted the line for.
		return FMath::Max(0.0, Want.StepEnd - T - F * 0.5);
	}

	if (Want.Claim.Resource.Kind == ETrafficResourceKind::Edge)
	{
		// The blocker's NEAREST boundary ahead, back in route distance. The mirror a
		// reversed step needs is FClaimGeometry::BoundaryAhead, which says why.
		const double Boundary = FClaimGeometry::BoundaryAhead(
			Want.StepStart, Want.EdgeLength, Want.bReversed, Blocker.From, Blocker.To);
		return FMath::Max(0.0, Boundary - T - G);
	}

	if (Want.bEndNode)
	{
		// Refused at the ENTRY to a box: stop a gap short of the box's START, outside
		// the junction, where this agent can still turn - never inside it, where nobody
		// can and where it would block the node behind it as well.
		//
		// AND A GAP SHORT OF WHERE THE NODE'S REACH BEGINS, not of the node: on a sweep
		// arc that hugs its taxiway, a gap short of the join is still beside the taxiway,
		// and the aircraft the node was refused for would pass through the waiter's wing.
		// Measured before this term existed: 429 uu at the closest, against a 1000 uu
		// footprint (Airside.Model.Traffic.DepartureMeetsArrivalOnTaxiway).
		return Want.bBoxEntry
			? FMath::Max(0.0, Want.StepStart - T - G)
			: FMath::Max(0.0, Want.StepEnd - Want.ReachExcess - T - G);
	}

	// The node the agent is STANDING on, refused. It cannot stop short of where it
	// already is, so the only honest answer is "do not move".
	//
	// ROUTINE, not exceptional: it fires whenever two agents' bodies are within half
	// a footprint of one node - a follower dispatched from the stand its leader has
	// not yet cleared (Airside.Model.Traffic.CarFollowing does exactly that), an
	// agent redirected onto a node somebody is crossing, or an aircraft parked on a
	// node a van drives over. It also fires for the node an agent has just left,
	// which is why the tail claim exists at all.
	//
	// AND IT CATCHES THE CROSSING SURFACE TOO, which is a SURFACE with no ESurface
	// tag: route zero's claim says "my body is on this strip", so a refusal there is
	// the same statement as a refused node - somebody else is standing where this
	// agent already is - and 0 is the same honest answer. The tagged RunwayEdge rule
	// above must not take it: that one stops an agent a gap short of the step's
	// start, which for ground the agent is already on would be a stop point behind it.
	return 0.0;
}

int32 FClaimPass::RunCallCountForTest = 0;

void FClaimPass::Run(FRoadAgent& Agent, const URoadNetwork& Network)
{
	// COUNTED FIRST, unconditionally, the same convention RebuildCountForTest and
	// MakeContextCallCountForTest use: every branch below returns early somewhere, and the
	// question this answers is "how many times did Run actually get called", not "how many
	// of those calls reached the bottom" - see RunCallCountForTest's own comment.
	++RunCallCountForTest;

	// NOT TAXIING: hold the runway and nothing else. An arrival on the roll and a departure
	// lining up own the strip; whatever either held on the taxiway before the handover is
	// released here, which is what makes "Vacated releases the chain" fall out of the tick
	// rather than needing a call of its own.
	//
	// A PUSH IS ON A ROUTE and must NOT take this arm. A manoeuvring aeroplane is standing on
	// its stand's own lead-in; releasing every guideline claim would show that ground free
	// with an aeroplane on it, and something could be cleared down the line it is being
	// pushed along. That is the same hole spec 3.4's crossing rule exists to close, in a
	// phase where the aeroplane is barely moving and entirely unable to get out of the way.
	//
	// SO IS A REVERSE (issue #434): a truck backing along a bay's reverse leg is standing on that
	// leg, and this arm read "not on a route" for it from 2026-09-17 - every claim dropped, the stop
	// distance unbounded - until IsOnRoute named the phase.
	if (!Agent.IsOnRoute())
	{
		HoldRunwayOnly(Agent, Network);
		ClaimGoalNode(Agent, Network);
		return;
	}

	const FRoutePlan& Plan = Agent.PlanInProgress();
	if (!Plan.IsValid() || Plan.Steps.Num() == 0)
	{
		ReleaseForDeadPlan(Agent);
		ClaimGoalNode(Agent, Network);
		return;
	}

	const FClaimWindow Window = WindowFor(Agent);

	// 0. THE RUNWAY THIS AGENT IS PHYSICALLY CROSSING - spec §3.1's fourth route, and the
	// one that closes the hole the other three leave. Once the tail passes a bar the bar
	// node leaves the window and its surface claim goes with it, while the agent is standing
	// on the centreline; and a junction's turn paths carry no DerivedFrom BY DESIGN, so the
	// crossing edges never imply the runway either. A landing could be cleared onto an
	// aircraft mid-crossing. So the crossing is held explicitly, from the bar until the tail
	// is geometrically clear.
	const FClaimBody Body = SampleBody(Plan, Window);
	UpdateCrossing(Agent, Network, Window, Body);

	// 1 AND 2. WHAT THE AGENT WANTS, IN ROUTE ORDER: the node the current step left, then
	// every step the window touches, with its edge interval, its end node and any runway
	// surface the two imply. Nothing is asked of the table yet.
	//
	// STILL A LOCAL, NOW ON THE INLINE BUFFER (issue #190) - see FPendingClaims's own
	// comment for why this stays a local rather than joining Wanted/OverlapsThisPass as a
	// member: FWantedClaim's forward declaration above blocks a member of this type, but not
	// a reference parameter of it, so TInlineAllocator<16> gets the usual handful of entries
	// off the heap without one.
	FPendingClaims Pending;
	BuildPending(Agent, Network, Window, Pending);

	// 3. ASK THE TABLE, in that order. The FIRST refusal decides how far the agent may go.
	ApplyClaims(Agent, Window, Pending);

	// 4. AND THE STAND IT IS GOING TO, after the route pass has released what is behind it.
	ClaimGoalNode(Agent, Network);
}

void FClaimPass::ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network)
{
	if (!Agent.GoalNode.IsSet() || Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
	{
		return;
	}
	// A DEAD PLAN HOLDS NOTHING, exactly as ReleaseForDeadPlan says: an agent whose route went
	// Unreachable is not at its goal and is not going there, so neither a reservation on the
	// stand nor an occupation of the goal node describes anything true. Airside.Model.Traffic.
	// DeadPlanReleases pins "the table holds NOTHING for it". An arrival's route is its
	// taxi-in, which the follower has not started yet.
	const FRoutePlan& Route = Agent.Phase == EAgentPhase::Arriving
		? Agent.TaxiInPlan : Agent.PlanInProgress();
	if (!Route.IsValid())
	{
		return;
	}
	const bool bParked = Agent.Phase == EAgentPhase::Parked;
	const bool bStandGoal = Network.FindEntityIndexByPoseNode(Agent.GoalNode) != INDEX_NONE;
	// A reservation names a STAND only: an aircraft heading for a runway or a plain node
	// reserves nothing ahead of itself beyond what the route pass already asks for. A parked
	// body occupies wherever it is.
	if (!bParked && (!bStandGoal || Agent.Class != ETraversalClass::Aircraft))
	{
		return;
	}
	// See FTrafficOccupancy::Assert for the WHY: a stand already held by someone else is a
	// planning failure upstream (the planner and the rebuild both skip held stands), and a
	// table cannot un-plan an aircraft.
	Table.Assert(FTrafficClaim::Make(Agent.Id, FTrafficResource::OfNode(Agent.GoalNode),
		bParked, TraversalPriority(Agent.Class)));
}

int32 FClaimPass::RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class)
{
	const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
	if (Found != nullptr && Found->PriorityOverride.Num() > 0)
	{
		// Scaled by ten so an authored order can never tie with a default one - a tie keeps
		// the holder, and an authored "vehicles first" that tied would mean nothing. A class
		// the author left out of the list ranks below everything named in it.
		const int32 Index = Found->PriorityOverride.Find(Class);
		return Index == INDEX_NONE ? 0 : 10 * (Found->PriorityOverride.Num() - Index);
	}
	return TraversalPriority(Class);
}

double FClaimPass::ReachExcessAt(const FTrafficRules& Rules, FNodeReachCache& Reach, const URoadNetwork& Network,
	FGuidelineNodeId Node, FGuidelineEdgeId Edge, ETraversalClass Class)
{
	// Per class because the footprint is: a van's reach along the same arc is shorter than
	// an aeroplane's, and the cache keys on the footprint it was asked for.
	const double F = Rules.FootprintFor(Class);
	return FMath::Max(0.0, Reach.Get(Network, Node, Edge, F) - F * 0.5);
}
