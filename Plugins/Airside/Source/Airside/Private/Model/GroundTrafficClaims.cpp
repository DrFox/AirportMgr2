// UGroundTraffic::ClaimAhead and the helpers it alone uses - spec 2026-09-06 §3.1-§3.3.
// One class across four translation units, the shape URoadEditFacade already uses
// (RoadEditFacade.cpp beside RoadEditFacadeSurfaces.cpp): the claim pass is the biggest
// single concern in this class and the one every traffic bug is read in, so it gets a file
// of its own rather than 740 lines in the middle of dispatch and tick. See
// Model/GroundTraffic.h for the class itself and for which file holds what.

// ClaimAhead itself is the numbered sequence its comments have always carried: the two
// early branches, the window, step 0's crossing, steps 1-2's wanted list, step 3's ask.
// Each numbered step is one function below, and ClaimAhead is last.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

/**
 * One thing an agent wants this tick, plus what the REFUSAL rule needs to know about it.
 *
 * The step geometry travels with the claim rather than being re-derived at the refusal,
 * because the refusal has to answer "how far may this agent go" in ROUTE distance while
 * the claim itself is in EDGE distance, and the map between them (which step, how long,
 * which way round) is exactly these fields. Re-deriving it from the blocker would mean a
 * second reading of the same step - the thing this codebase calls a second evaluator.
 */
struct UGroundTraffic::FWantedClaim
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

	/** The node carrying the bar, for the log line. Set only when Surface == HoldingPosition;
	*  the segment it protects is already on Claim.Resource. */
	FGuidelineNodeId HoldNode;
};

void UGroundTraffic::HoldRunwayOnly(FRoadAgent& Agent, const URoadNetwork& Network)
{
	// CLEARED, or the arbitration fields say whatever they said on the last tick this
	// agent taxied. A parked aircraft still naming the vehicle it once queued behind
	// would feed Task 8's wait-for graph an edge out of an agent that is not waiting for
	// anything, and a cycle through it would be a phantom nobody could resolve.
	Agent.StopWithin = TNumericLimits<double>::Max();
	Agent.WaitingOn = 0;
	Agent.BlockedStep = INDEX_NONE;
	Agent.LastOverlaps.Reset();

	TArray<FTrafficResource> Surfaces;
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
	// one case that matters: an aircraft whose plan dies mid-crossing is Parked by the end
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
		TArray<FRoadSegmentId> Chain = Network.RunwayChain(Agent.CrossingRunway);
		if (Chain.Num() == 0)
		{
			Chain.Add(Agent.CrossingRunway);
		}
		for (const FRoadSegmentId Segment : Chain)
		{
			Surfaces.AddUnique(FTrafficResource::OfSurface(Segment));
		}
	}

	Occupancy.ReleaseExcept(Agent.Id, Surfaces);

	for (const FTrafficResource& Resource : Surfaces)
	{
		FTrafficClaim Claim;
		Claim.AgentId = Agent.Id;
		Claim.Resource = Resource;
		Claim.bOccupied = true;
		Claim.Rank = TraversalPriority(Agent.Class);

		// The result is not acted on: an aircraft already rolling cannot be told to stop
		// by a table, and DispatchArrival refused the landing at the door if the chain
		// was held (see ArrivalPlanner::Plan). Who is allowed onto a runway NEXT is
		// URunwaySequencer's question in M3, asked before anything is dispatched.
		FTrafficClaim Blocker;
		Occupancy.TryClaim(Claim, Blocker);
	}
}

void UGroundTraffic::ReleaseForDeadPlan(FRoadAgent& Agent)
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
	Occupancy.ReleaseGuidelineClaimsOf(Agent.Id);

	// THE CROSSING FIELDS SURVIVE WITH THE CLAIM THEY DESCRIBE. Clearing them here (which
	// is what this did, in the breath that also dropped the claim) would leave the phase
	// saying the agent is off the strip while the table says it is on it - and the release
	// line it logged would have been a log that lies.
	//
	// AND THEY GO ON SURVIVING. An invalid plan makes FRouteFollower::HasArrived true, so
	// this agent is Parked by the end of this very tick and takes HoldRunwayOnly from the
	// next one - which re-claims the crossing chain for exactly this reason. The hold ends
	// where the agent does: RetireAgent, or Advance's removal path. A player who deletes the
	// taxiway under a crossing aeroplane has an aeroplane on the runway, and the table says
	// so until they retire it.

	Agent.StopWithin = TNumericLimits<double>::Max();
	Agent.WaitingOn = 0;
	Agent.BlockedStep = INDEX_NONE;
	Agent.LastOverlaps.Reset();
}

UGroundTraffic::FClaimWindow UGroundTraffic::WindowFor(const FRoadAgent& Agent) const
{
	const FRoutePlan& Plan = Agent.Follower.Plan;

	const double T = Agent.Follower.Travelled;
	const double F = Rules.FootprintFor(Agent.Class);
	const double G = Rules.GapFor(Agent.Class);

	// The follower's own braking figure, not the rules': the window has to be the distance
	// THIS airframe needs, or an agent reserves less line than it can stop in.
	const double Decel = FMath::Max(KINDA_SMALL_NUMBER, Agent.Follower.Ground.Taxi.Decel);
	const double Window = Agent.Follower.Speed * Agent.Follower.Speed / (2.0 * Decel) + G;

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
	const double Head = T + F * 0.5 + Window;
	const double Tail = T - F * 0.5;
	const int32 Current = CurrentStep(Plan, T);

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

UGroundTraffic::FClaimBody UGroundTraffic::SampleBody(const FRoutePlan& Plan, const FClaimWindow& Window)
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
	// direction, which is a property of the array and not of the distance asked for.
	FClaimBody Body;
	double Bearing = 0.0;
	Body.bValid =
		GuidelineGeom::PointAtDistance(Plan.Polyline, T + F * 0.5, Body.Nose, Bearing)
		&& GuidelineGeom::PointAtDistance(Plan.Polyline, T, Body.Centre, Bearing)
		&& GuidelineGeom::PointAtDistance(Plan.Polyline, T - F * 0.5, Body.Tail, Bearing);
	return Body;
}

void UGroundTraffic::UpdateCrossing(FRoadAgent& Agent, const URoadNetwork& Network,
	const FClaimWindow& Window, const FClaimBody& Body) const
{
	const FRoutePlan& Plan = Agent.Follower.Plan;
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

	const FGuidelineNodeId FromId = StepFromNode(Plan, Current);
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
		bool bOntoStrip = Network.IsGuidelineNodeOnRunway(Plan.Steps[Current].To, Bar);

		if (!bOntoStrip)
		{
			// EndVertex is this step's last point in the plan's welded polyline, and the
			// previous step's is its first - the map the plan already carries, rather than
			// a second walk over distances.
			const int32 FirstVertex = Current > 0 ? Plan.Steps[Current - 1].EndVertex : 0;
			const int32 LastVertex = FMath::Min(Plan.Steps[Current].EndVertex, Plan.Polyline.Num() - 1);
			for (int32 Vertex = FMath::Max(0, FirstVertex); Vertex <= LastVertex && !bOntoStrip; ++Vertex)
			{
				bOntoStrip = Network.IsPointOnRunway(Plan.Polyline[Vertex], Bar);
			}
		}

		if (!bOntoStrip)
		{
			// FALLBACK ON LastMotion only when the polyline cannot be sampled at all. It
			// is a tick stale, which is why it is not the first choice.
			bOntoStrip = bHaveBody
				? Network.IsPointOnRunway(NosePoint, Bar)
				: Network.IsPointOnRunway(Agent.LastMotion.Position, Bar);
		}

		if (bOntoStrip)
		{
			Agent.CrossingRunway = Bar;
			Agent.CrossingPhase = ECrossingPhase::Committed;
		}
	}

	// 0b. ON THE STRIP once the CENTRE is - checked every tick while Committed, and on
	// whatever step the agent has reached by then: a bar may sit a step or more short of
	// the asphalt, and the crossing does not begin at the bar, it begins at the edge of
	// the surface.
	if (Agent.CrossingPhase == ECrossingPhase::Committed && Agent.CrossingRunway.IsSet()
		&& bHaveBody && Network.IsPointOnRunway(CentrePoint, Agent.CrossingRunway))
	{
		Agent.CrossingPhase = ECrossingPhase::OnStrip;
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
		bool bClear = false;
		if (bHaveBody)
		{
			// NO PART OF THE BODY STILL ON IT - nose, centre and tail. "The tail is off
			// the strip" ALONE is not the test and was tried: it is true on the way IN as
			// well, so the hold ended 5000 uu early, with the aeroplane about to drive
			// onto the asphalt (measured at route 17825 on the two-bar crossing, where the
			// tail is off the strip while the nose is already on it).
			const bool bBodyClear =
				!Network.IsPointOnRunway(NosePoint, Agent.CrossingRunway)
				&& !Network.IsPointOnRunway(CentrePoint, Agent.CrossingRunway)
				&& !Network.IsPointOnRunway(TailPoint, Agent.CrossingRunway);

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
			const bool bNodeOnStrip = Network.IsGuidelineNodeOnRunway(FromId, Agent.CrossingRunway, &ChainHalfWidth);
			const double TailPast = (T - F * 0.5) - StepStart(Plan, Current);
			bClear = !bFromIsBar && TailPast >= 0.0 && (!bNodeOnStrip || TailPast > ChainHalfWidth);
		}

		if (bClear)
		{
			Agent.CrossingRunway = FRoadSegmentId();
			Agent.CrossingPhase = ECrossingPhase::None;

			// THE RELEASE LINE LIVES HERE, not at the Vacated handover where it started:
			// vacating no longer gives the runway back, it hands it to this rule, and a
			// line claiming otherwise would be a log that lies. Transition by
			// construction - the phase is None immediately after.
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Agent.Id);
		}
	}
}

void UGroundTraffic::BuildPending(const FRoadAgent& Agent, const URoadNetwork& Network,
	const FClaimWindow& Window, TArray<FWantedClaim>& Pending) const
{
	const FRoutePlan& Plan = Agent.Follower.Plan;
	const double T = Window.T;
	const double F = Window.F;
	const double G = Window.G;
	const double Head = Window.Head;
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
	// released. See ClaimAhead for why the runway under a crossing agent is held at all.
	// AT THE FRONT of Pending, so a refusal here binds before anything further along the
	// route: the agent is standing on this surface, and nothing it might be told about a
	// node ahead can matter more than that. COMMITTED COUNTS AS STANDING ON IT: the body
	// is a moment from the asphalt and nothing may be cleared onto it in between.
	if (Agent.CrossingPhase != ECrossingPhase::None && Agent.CrossingRunway.IsSet())
	{
		TArray<FRoadSegmentId> Chain = Network.RunwayChain(Agent.CrossingRunway);
		if (Chain.Num() == 0)
		{
			Chain.Add(Agent.CrossingRunway);
		}
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
			Crossing.StepStart = StepStart(Plan, Current);
			Crossing.StepEnd = Plan.Steps[Current].EndDistance;
			Pending.Add(Crossing);
		}
	}

	// THE ENTRY RULE FIRES FOR ONE BOX PER PASS - the first the window reaches that the agent
	// has not entered. See the loop below and ClaimAhead's header for the trace that rejected
	// chaining it through every consecutive box.
	bool bBoxEntryTaken = false;

	// 1. THE NODE THE CURRENT STEP LEFT, while the centre is still within half a footprint
	// of it. Without this an agent that had just crossed a junction would release it with
	// its tail still inside, and the next claimant would drive into that tail.
	if (T - StepStart(Plan, Current) < F * 0.5)
	{
		const FGuidelineNodeId From = StepFromNode(Plan, Current);
		FWantedClaim Want;
		Want.Claim.AgentId = Agent.Id;
		Want.Claim.Resource = FTrafficResource::OfNode(From);
		Want.Claim.bOccupied = true;
		Want.Claim.Rank = RankAt(Network, From, Agent.Class);
		Want.Step = Current;
		Want.StepStart = StepStart(Plan, Current);
		Want.StepEnd = Plan.Steps[Current].EndDistance;
		Pending.Add(Want);
	}

	// 2. Forward along the steps while the window still has distance left.
	for (int32 Index = Current; Index < Plan.Steps.Num(); ++Index)
	{
		const FRouteStep& Step = Plan.Steps[Index];
		const double Start = StepStart(Plan, Index);
		if (Start >= Head)
		{
			break;
		}
		const double End = Step.EndDistance;
		const double Length = End - Start;

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
				? RankAt(Network, StepFromNode(Plan, Current), Agent.Class)
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
				for (const FRoadSegmentId Segment : Network.RunwayChain(Edge->DerivedFrom))
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

		if (End < Head || bBoxEntry)
		{
			FWantedClaim Want;
			Want.Claim.AgentId = Agent.Id;
			Want.Claim.Resource = FTrafficResource::OfNode(Step.To);

			// Occupied only while the CENTRE is within half a footprint of the node, which
			// is the same test the left-behind node above uses, read forwards.
			Want.Claim.bOccupied = FMath::Abs(End - T) < F * 0.5;
			Want.Claim.Rank = RankAt(Network, Step.To, Agent.Class);
			Want.Step = Index;
			Want.bEndNode = true;
			Want.bBoxEntry = bBoxEntry;
			Want.StepStart = Start;
			Want.StepEnd = End;
			Want.EdgeLength = Length;
			Want.bReversed = Step.bReversed;
			Pending.Add(Want);

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
				// protecting a piece of what it used to.
				TArray<FRoadSegmentId> Chain = Network.RunwayChain(Node->HoldingPositionFor);
				if (Chain.Num() == 0)
				{
					Chain.Add(Node->HoldingPositionFor);
				}

				for (const FRoadSegmentId Segment : Chain)
				{
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
					// measures it with a bar on each side of the runway.
					if (WantedOccupied(Bar.Claim.Resource))
					{
						continue;
					}
					Pending.Add(Bar);
				}
			}
		}
	}
}

void UGroundTraffic::ApplyClaims(FRoadAgent& Agent, const FClaimWindow& Window,
	const TArray<FWantedClaim>& Pending)
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
	// time. Releasing everything and re-claiming is still rejected (ClaimAhead's header):
	// this drops only what was not asked for.
	TArray<FTrafficResource> Wanted;
	Wanted.Reserve(Pending.Num());

	const int32 WasWaitingOn = Agent.WaitingOn;
	const int32 WasBlockedStep = Agent.BlockedStep;
	bool bHeld = false;

	// Everyone this pass overlapped, for the Warning's throttle. See FRoadAgent::LastOverlaps.
	TArray<int32> OverlapsThisPass;

	for (const FWantedClaim& Want : Pending)
	{
		// PAST THE FIRST REFUSAL, only ground the agent occupies is still claimed. Skipping
		// its own occupancies here was a defect: an agent refused a node it was STANDING on
		// abandoned the rest of its own body, and the agent that had merely reserved that
		// node drove into it.
		if (bHeld && !Want.Claim.bOccupied)
		{
			continue;
		}

		FTrafficClaim Blocker;
		const bool bGranted = Occupancy.TryClaim(Want.Claim, Blocker) == EClaimResult::Granted;
		if (bGranted || Want.Claim.bOccupied)
		{
			Wanted.Add(Want.Claim.Resource);
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
			if (!Agent.LastOverlaps.Contains(Blocker.AgentId))
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
		Agent.BlockedStep = Want.Step;
		Agent.BlockedResource = Want.Claim.Resource;

		// A BAR-HOLDER'S BLOCKED STEP IS THE ONE LEAVING THE BAR, not the one arriving at
		// it. The refusal was raised for the step that ENDS at the bar node, and the agent
		// stops with its nose on the bar - which is AT the start of the next step. The
		// deadlock resolver asks "is this agent at the node its refused step leaves from",
		// and measured against the arriving step a departure waiting at a bar was never a
		// candidate: the one member of a bar-versus-arrival cycle who could have turned was
		// never asked (PIE, 2026-09-06). Only when there IS a next step; a bar at the end of
		// a route is the route's end.
		if (Want.Surface == FWantedClaim::ESurface::HoldingPosition
			&& Agent.Follower.Plan.Steps.IsValidIndex(Want.Step + 1))
		{
			Agent.BlockedStep = Want.Step + 1;
		}
		// The transition tests below compare against what was STORED last pass, so they
		// read the stored value, not Want.Step - a bar refusal stores the next step, and
		// comparing the raw one logged "holding short" on every tick.
		const int32 NewBlockedStep = Agent.BlockedStep;

		Agent.StopWithin = StopWithinFor(Want, Blocker, Window);

		// ON THE TRANSITION ONLY. Logged every tick this would be one line per agent per
		// frame, which is how a log stops being read at all.
		//
		// A BAR GETS ITS OWN LINE INSTEAD OF THE GENERIC ONE - spec §10 lists "holding-position
		// reached" separately from "stopped for a resource" - because it names the node
		// and the segment as well as the holder, and one event must not produce two
		// lines. Its transition test is the blocker OR the step: an agent already waiting
		// on the same holder for something else is still newly held HERE, and BlockedStep
		// is what changed when it became so.
		if (Want.Surface == FWantedClaim::ESurface::HoldingPosition)
		{
			if (WasWaitingOn != Blocker.AgentId || WasBlockedStep != NewBlockedStep)
			{
				UE_LOG(LogAirsideTraffic, Log,
					TEXT("Agent %d holding short at node %d for runway segment %d held by agent %d"),
					Agent.Id, Want.HoldNode.Index, Want.Claim.Resource.Surface.Index, Blocker.AgentId);
			}
		}
		else if (WasWaitingOn != Blocker.AgentId)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d stops %.0f uu short of %s held by agent %d"),
				Agent.Id, Agent.StopWithin, *Blocker.Resource.Describe(), Blocker.AgentId);
		}
		Agent.WaitingOn = Blocker.AgentId;

		// NO break: the loop above skips every RESERVATION past this point (claiming line
		// beyond a refusal would hold it against everybody for a journey the agent is not
		// making this tick) but must go on claiming the ground the agent is standing on.
	}

	Agent.LastOverlaps = MoveTemp(OverlapsThisPass);
	Occupancy.ReleaseExcept(Agent.Id, Wanted);

	if (!bHeld)
	{
		Agent.StopWithin = TNumericLimits<double>::Max();
		Agent.WaitingOn = 0;
		Agent.BlockedStep = INDEX_NONE;
		if (WasWaitingOn != 0)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d resumes"), Agent.Id);
		}
	}
}

double UGroundTraffic::StopWithinFor(const FWantedClaim& Want, const FTrafficClaim& Blocker,
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
		return Want.bBoxEntry
			? FMath::Max(0.0, Want.StepStart - T - G)
			: FMath::Max(0.0, Want.StepEnd - T - G);
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

void UGroundTraffic::ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network)
{
	// NOT TAXIING: hold the runway and nothing else. An arrival on the roll and a departure
	// lining up own the strip; whatever either held on the taxiway before the handover is
	// released here, which is what makes "Vacated releases the chain" fall out of the tick
	// rather than needing a call of its own.
	if (Agent.Phase != EAgentPhase::Taxiing)
	{
		HoldRunwayOnly(Agent, Network);
		return;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (!Plan.IsValid() || Plan.Steps.Num() == 0)
	{
		ReleaseForDeadPlan(Agent);
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
	TArray<FWantedClaim> Pending;
	BuildPending(Agent, Network, Window, Pending);

	// 3. ASK THE TABLE, in that order. The FIRST refusal decides how far the agent may go.
	ApplyClaims(Agent, Window, Pending);
}
