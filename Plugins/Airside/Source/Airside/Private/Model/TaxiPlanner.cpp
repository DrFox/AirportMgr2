#include "Model/TaxiPlanner.h"

#include "Algo/Reverse.h"
#include "Model/Airframe.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteEdgeFilter.h"
#include "Model/SpeedProfile.h"
#include "Model/TrafficRules.h"

namespace
{
	/**
	 * The most edges one MOVE may run through nodes it may not stop at (see FSippState). A junction's turn path is
	 * one to three pieces (Chord, TaperS, BendArc), a crossing's split adds a piece per conflict; eight covers the
	 * longest chain FRoadGuidelineBuilder lays, with room. A longer chain is not explored - the planner then finds
	 * no way through it and says NoFreeWindow where RouteSearch found a route: a refusal, never a plan that stops
	 * inside a junction.
	 */
	constexpr int32 MaxChainEdges = 8;

	/** One edge of a move, Enter and Reach as offsets from the move's departure. */
	struct FChainStep
	{
		FGuidelineEdgeId Edge;
		bool bReversed = false;
		FGuidelineNodeId To;
		double Enter = 0.0;
		double Reach = 0.0;

		/** Entered from rest: the move's first step when it sets off from rest, or the step after a sharp joint. */
		bool bFromRest = false;
	};

	/**
	 * One SIPP search state - ALWAYS AT A NODE THE AIRCRAFT MAY STOP AT (the start, a CanHoldAt node, or the goal),
	 * inside one of its free intervals, having arrived along InEdge inside one of THAT edge's free intervals.
	 *
	 * WHY ONLY THERE, and why the search moves in CHAINS between such nodes: textbook SIPP (Phillips & Likhachev
	 * 2011) keys a state on (node, interval) and lets the earliest arrival dominate, because an aircraft that
	 * arrives early can always wait. Inside a junction it may not wait, so an early arrival at a junction node does
	 * NOT dominate a later one - measured 2026-10-02 on WaitsAtPlainNodeNotInJunction: the early arrival at J1 was a
	 * dead end (its way out was booked) and, keyed alike, it discarded the later arrival that would have fitted. So
	 * a move runs from a stoppable node through every no-stop node to the next stoppable one as ONE step, with ONE
	 * departure time that fits the whole chain - and states exist only where waiting, and so dominance, holds.
	 *
	 * THE ARRIVING EDGE AND ITS INTERVAL ARE IN THE KEY, which textbook SIPP's are not: while the aircraft waits it
	 * still holds that edge with its tail, so how long it may wait is the edge's interval as much as the node's.
	 */
	struct FSippState
	{
		FGuidelineNodeId Node;
		int32 NodeInterval = 0;
		FGuidelineEdgeId InEdge;
		bool bInReversed = false;
		int32 EdgeInterval = 0;

		/** Whether InEdge was entered from rest - which of its timings a stop here is measured from. */
		bool bInFromRest = false;

		/** When it reaches Node - rolling, or at rest when it is the start. */
		double Arrive = 0.0;

		/** The last moment it may leave Node: its node and its tail's edge stay free until this plus the margin. */
		double LatestLeave = 0.0;

		/** When the move that brought it here set off from the previous state's node. */
		double Left = 0.0;

		/** That move's edges. */
		TArray<FChainStep> Chain;

		int32 Parent = INDEX_NONE;
	};

	struct FSippKey
	{
		FGuidelineNodeId Node;
		int32 NodeInterval = 0;
		FGuidelineEdgeId InEdge;
		int32 EdgeInterval = 0;

		bool operator==(const FSippKey& Other) const
		{
			return Node == Other.Node && NodeInterval == Other.NodeInterval && InEdge == Other.InEdge
				&& EdgeInterval == Other.EdgeInterval;
		}
	};

	uint32 GetTypeHash(const FSippKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.Node), ::GetTypeHash(Key.NodeInterval)),
			HashCombine(GetTypeHash(Key.InEdge), ::GetTypeHash(Key.EdgeInterval)));
	}

	bool ByEstimate(const TPair<double, int32>& A, const TPair<double, int32>& B)
	{
		return A.Key < B.Key;
	}

	/** A resource a move needs free over [Tau + From, Tau + To], Tau its departure. To == Forever: from then on. */
	struct FNeed
	{
		FTaxiResource Resource;
		double From = 0.0;
		double To = 0.0;
	};

	/**
	 * The least Shift >= 0 such that [Lo + Shift, Hi + Shift] lies in one of Free's intervals, and which one; false
	 * when none can hold it. Free is sorted and disjoint, so the first interval that fits gives the least shift.
	 * Hi == Forever fits only an open-ended interval.
	 */
	bool LeastShift(const TArray<FTaxiInterval>& Free, double Lo, double Hi, double& OutShift, int32& OutIndex)
	{
		for (int32 Index = 0; Index < Free.Num(); ++Index)
		{
			const FTaxiInterval& Interval = Free[Index];
			const double Shift = FMath::Max(0.0, Interval.Start - Lo);
			const bool bOpenEnded = Interval.End >= FTaxiReservations::Forever;
			const bool bFits = Hi >= FTaxiReservations::Forever ? bOpenEnded : (bOpenEnded || Hi + Shift <= Interval.End);
			if (bFits)
			{
				OutShift = Shift;
				OutIndex = Index;
				return true;
			}
		}
		return false;
	}
}

FTaxiPlanner::FTaxiPlanner(const URoadNetwork& InNetwork, const FTaxiReservations& InTable, const FAirframe& InAirframe,
	const FTrafficRules& InRules)
	: Network(InNetwork)
	, Table(InTable)
	, Airframe(InAirframe)
	, Rules(InRules)
{
}

const TArray<FVector2D>& FTaxiPlanner::SamplesOf(FGuidelineEdgeId Edge, bool bReversed)
{
	const TPair<FGuidelineEdgeId, bool> Key(Edge, bReversed);
	if (const TArray<FVector2D>* Known = EdgeSamples.Find(Key))
	{
		return *Known;
	}
	TArray<FVector2D> Points;
	// SampleGuideline, the one sampler: the array the follower will walk, never a second evaluation of the curve.
	if (!Network.SampleGuideline(Edge, Points, bReversed))
	{
		Points.Reset();
	}
	return EdgeSamples.Add(Key, MoveTemp(Points));
}

const FTaxiEdgeSeconds& FTaxiPlanner::SecondsFor(FGuidelineEdgeId Edge, bool bReversed)
{
	const TPair<FGuidelineEdgeId, bool> Key(Edge, bReversed);
	if (const FTaxiEdgeSeconds* Known = EdgeSeconds.Find(Key))
	{
		return *Known;
	}

	FTaxiEdgeSeconds Seconds;
	const TArray<FVector2D> Points = SamplesOf(Edge, bReversed);
	const FChassis& Chassis = Airframe.Chassis;
	if (Points.Num() >= 2 && Chassis.Ground.IsSet())
	{
		// THE AUTHORITY, PER PIECE: the rules the follower's whole-route profile applies, over this edge's own samples.
		// Two builds give the four ways an edge is driven - rolling or at rest at each end. Entering "rolling" is at
		// the piece's own limit there; across an edge boundary that skips the braking a NEXT edge's bend asks of this
		// one - the planner's one approximation, bounded by Airside.Model.TaxiPlan.EtaAgreesWithWholeRouteProfile.
		// An instant corner AT the boundary is not skipped: IsSharpJoint times it as a stop.
		FSpeedProfile Rolls;
		Rolls.BuildPiece(Points, Chassis, EPieceEnd::Rolls);
		FSpeedProfile Stops;
		Stops.BuildPiece(Points, Chassis, EPieceEnd::Stops);
		Seconds.RollRoll = Rolls.SecondsToDrive(Rolls.LimitAt(0.0));
		Seconds.RestRoll = Rolls.SecondsToDrive(0.0);
		Seconds.RollRest = Stops.SecondsToDrive(Stops.LimitAt(0.0));
		Seconds.RestRest = Stops.SecondsToDrive(0.0);
		Seconds.bUsable = !Rolls.IsEmpty() && !Stops.IsEmpty();
	}
	return EdgeSeconds.Add(Key, Seconds);
}

bool FTaxiPlanner::IsSharpJoint(FGuidelineEdgeId In, bool bInReversed, FGuidelineEdgeId Out, bool bOutReversed)
{
	const TTuple<FGuidelineEdgeId, bool, FGuidelineEdgeId, bool> Key(In, bInReversed, Out, bOutReversed);
	if (const bool* Known = SharpJoints.Find(Key))
	{
		return *Known;
	}

	bool bSharp = false;
	const TArray<FVector2D> Before = SamplesOf(In, bInReversed);
	const TArray<FVector2D> After = SamplesOf(Out, bOutReversed);
	if (Before.Num() >= 2 && After.Num() >= 2)
	{
		// The span into the joint and the span out of it, welded at the shared node as RouteSearch welds them, and
		// the authority asked whether that vertex turns instantly. Quiet: a piece.
		const TArray<FVector2D> Joint = { Before[Before.Num() - 2], Before.Last(), After[1] };
		FSpeedProfile Probe;
		Probe.BuildPiece(Joint, Airframe.Chassis, EPieceEnd::Rolls);
		bSharp = Probe.HasSharpVertex();
	}
	return SharpJoints.Add(Key, bSharp);
}

double FTaxiPlanner::RouteSeconds(const FRoutePlan& Route)
{
	double Total = 0.0;
	const int32 Count = Route.Steps.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FRouteStep& Step = Route.Steps[Index];
		const FTaxiEdgeSeconds& Seconds = SecondsFor(Step.Edge, Step.bReversed);
		const bool bFromRest = Index == 0
			|| IsSharpJoint(Route.Steps[Index - 1].Edge, Route.Steps[Index - 1].bReversed, Step.Edge, Step.bReversed);
		const bool bToRest = Index == Count - 1
			|| IsSharpJoint(Step.Edge, Step.bReversed, Route.Steps[Index + 1].Edge, Route.Steps[Index + 1].bReversed);
		Total += bFromRest ? (bToRest ? Seconds.RestRest : Seconds.RestRoll) : (bToRest ? Seconds.RollRest : Seconds.RollRoll);
	}
	return Total;
}

bool FTaxiPlanner::CanHoldAt(const URoadNetwork& InNetwork, const FTrafficRules& InRules, FGuidelineEdgeId Arrived,
	FGuidelineNodeId At)
{
	const FGuidelineEdge* Edge = InNetwork.GetGuidelineEdge(Arrived);
	const FGuidelineNode* Node = InNetwork.GetGuidelineNode(At);
	if (Edge == nullptr || Node == nullptr)
	{
		return false;
	}

	// INSIDE A JUNCTION, NEVER (spec §1): an aircraft that stops on a turn path blocks every line through the
	// junction, and its tail the node behind. A turn path is what the builder laid at a junction (AtJunction); a box
	// is any step too short to stand on clear of the node behind - the claim pass's own rule, asked, not copied. A
	// crossing's conflict node is where a road and a taxiway contend: stopping on it holds the road.
	return !Edge->AtJunction.IsSet()
		&& !InRules.IsBox(Edge->Length, ETraversalClass::Aircraft)
		&& !Node->bCrossingConflict;
}

FTaxiPlan FTaxiPlanner::Plan(const FTaxiRequest& Request)
{
	FTaxiPlan Out;

	// RouteSearch's query, built by its one factory, so the policy (runway avoidance) and the size gates are the
	// ones a RouteSearch::Find for this errand would apply.
	const FRouteQuery Query = FRouteQuery::For(Request.Errand, Request.Start, Request.Goal, Airframe.Wingspan,
		ETraversalClass::Aircraft).NeedsPavement(Airframe.MinimumPavement);

	// RouteSearch's own refusal, logged by it once. Find is NOT asked afterwards: it would log the same refusal
	// twice. RouteResult stays NoStart, which is what Find returns for an unanswerable query.
	if (!RouteSearch::IsQueryAnswerable(Query))
	{
		return Out;
	}

	// WHY NOT is RouteSearch's to say, on any failure: a static search for the same query tells a layout problem
	// (no route - and which: one-way, too wide, unconnected) from a traffic one (a route, but no free window).
	auto Refuse = [this, &Query, &Out]()
	{
		Out.RouteResult = RouteSearch::Find(Network, Query).Result;
		Out.Result = Out.RouteResult == ERouteResult::Found ? ETaxiPlanResult::NoFreeWindow : ETaxiPlanResult::NoRoute;
		return Out;
	};

	if (Request.Start == Request.Goal || Network.GetGuidelineNode(Request.Start) == nullptr
		|| Network.GetGuidelineNode(Request.Goal) == nullptr)
	{
		return Refuse();
	}

	const double Margin = FMath::Max(0.0, Rules.TaxiPlanMargin);
	const FVector2D GoalAt = Network.GetGuidelineNode(Request.Goal)->Position;
	const double TopSpeed = Airframe.Chassis.Ground.Taxi.SpeedCap;

	// Straight-line distance at the taxi cap: never more than the real time, because no piece is driven faster than
	// the cap (FSpeedProfile caps every span at it). So the first goal popped is the earliest arrival.
	auto Heuristic = [this, &GoalAt, TopSpeed](FGuidelineNodeId Node)
	{
		const FGuidelineNode* At = Network.GetGuidelineNode(Node);
		return (At != nullptr && TopSpeed > 0.0) ? FVector2D::Distance(At->Position, GoalAt) / TopSpeed : 0.0;
	};

	// The table's free intervals per resource, once per plan: every move through a resource asks for the same ones.
	// Copied out rather than referenced: the map grows while callers hold what it returned.
	TMap<FTaxiResource, TArray<FTaxiInterval>> FreeCache;
	auto FreeOf = [this, &FreeCache, &Request](const FTaxiResource& Resource) -> TArray<FTaxiInterval>
	{
		if (const TArray<FTaxiInterval>* Known = FreeCache.Find(Resource))
		{
			return *Known;
		}
		TArray<FTaxiInterval> Fresh;
		Table.FreeIntervals(Resource, Request.Holder, Fresh);
		FreeCache.Add(Resource, Fresh);
		return Fresh;
	};

	// The stop's own cost at a state: driving its arriving edge to rest instead of rolling off it - the brake, on the
	// edge just driven. The start is at rest already.
	auto StopLossAt = [this](const FSippState& State)
	{
		if (!State.InEdge.IsSet())
		{
			return 0.0;
		}
		const FTaxiEdgeSeconds& In = SecondsFor(State.InEdge, State.bInReversed);
		return FMath::Max(0.0, State.bInFromRest ? In.RestRest - In.RestRoll : In.RollRest - In.RollRoll);
	};

	// THE START: at rest on Start from DepartAt, in whichever free interval of the node contains that moment. None
	// means somebody else holds the node the aircraft is standing on - the table disagrees with the world, and the
	// honest answer is no plan rather than one that books over them.
	const TArray<FTaxiInterval> StartFree = FreeOf(FTaxiResource::Node(Request.Start));
	const int32 StartInterval = StartFree.IndexOfByPredicate([&Request](const FTaxiInterval& Interval)
	{
		return Interval.Start <= Request.DepartAt && Request.DepartAt < Interval.End;
	});
	if (StartInterval == INDEX_NONE)
	{
		return Refuse();
	}

	TArray<FSippState> States;
	TMap<FSippKey, double> BestArrive;
	TSet<FSippKey> Closed;
	TArray<TPair<double, int32>> Open;
	{
		FSippState Start;
		Start.Node = Request.Start;
		Start.NodeInterval = StartInterval;
		Start.Arrive = Request.DepartAt;
		Start.LatestLeave = StartFree[StartInterval].End - Margin;
		Start.Left = Request.DepartAt;
		States.Add(MoveTemp(Start));
		Open.HeapPush(TPair<double, int32>(Request.DepartAt + Heuristic(Request.Start), 0), ByEstimate);
	}

	FRouteEdgeFilter Filter(Network, Query);

	// Every chain of edges from a node through no-stop nodes to the next node the aircraft may stop at, or the goal.
	// Depth-first, never revisiting a node within one chain. The edges admitted are RouteSearch's (Filter).
	TArray<TArray<FChainStep>> Chains;
	TFunction<void(FGuidelineNodeId, TArray<FChainStep>&, TSet<FGuidelineNodeId>&)> Walk;
	Walk = [&](FGuidelineNodeId From, TArray<FChainStep>& Path, TSet<FGuidelineNodeId>& Visited)
	{
		TArray<FAdmittedEdge> Leaving;
		Filter.ForEachAdmitted(From, [&Leaving](const FAdmittedEdge& Admitted) { Leaving.Add(Admitted); });
		for (const FAdmittedEdge& Admitted : Leaving)
		{
			if (Visited.Contains(Admitted.Next) || !SecondsFor(Admitted.Id, Admitted.bReversed).bUsable)
			{
				continue;
			}
			FChainStep Step;
			Step.Edge = Admitted.Id;
			Step.bReversed = Admitted.bReversed;
			Step.To = Admitted.Next;
			Path.Add(Step);
			if (Admitted.Next == Request.Goal || CanHoldAt(Network, Rules, Admitted.Id, Admitted.Next))
			{
				Chains.Add(Path);
			}
			else if (Path.Num() < MaxChainEdges)
			{
				Visited.Add(Admitted.Next);
				Walk(Admitted.Next, Path, Visited);
				Visited.Remove(Admitted.Next);
			}
			Path.Pop();
		}
	};

	int32 Reached = INDEX_NONE;
	while (Open.Num() > 0)
	{
		TPair<double, int32> Top;
		Open.HeapPop(Top, ByEstimate);
		const FSippState S = States[Top.Value];
		const FSippKey Key{ S.Node, S.NodeInterval, S.InEdge, S.EdgeInterval };
		if (Closed.Contains(Key))
		{
			continue;
		}
		Closed.Add(Key);

		if (S.Node == Request.Goal)
		{
			Reached = Top.Value;
			break;
		}

		const bool bAtStart = !S.InEdge.IsSet();
		const double StopLoss = StopLossAt(S);

		Chains.Reset();
		{
			TArray<FChainStep> Path;
			TSet<FGuidelineNodeId> Visited;
			Visited.Add(S.Node);
			Walk(S.Node, Path, Visited);
		}

		for (TArray<FChainStep>& Chain : Chains)
		{
			// TWO WAYS TO SET OFF: rolling straight on (not from the start, nor round an instant corner), or from rest
			// after a stop here - waiting as long as the node and the tail's edge allow.
			for (int32 Variant = 0; Variant < 2; ++Variant)
			{
				const bool bRolling = Variant == 0;
				if (bRolling && (bAtStart || IsSharpJoint(S.InEdge, S.bInReversed, Chain[0].Edge, Chain[0].bReversed)))
				{
					continue;
				}

				// Offsets along the chain from the departure: each step's duration from the authority's four timings -
				// at rest after a stop or a sharp joint, at rest into a sharp joint or the goal.
				double Clock = 0.0;
				for (int32 Index = 0; Index < Chain.Num(); ++Index)
				{
					FChainStep& Step = Chain[Index];
					const FTaxiEdgeSeconds& Seconds = SecondsFor(Step.Edge, Step.bReversed);
					Step.bFromRest = Index == 0 ? !bRolling
						: IsSharpJoint(Chain[Index - 1].Edge, Chain[Index - 1].bReversed, Step.Edge, Step.bReversed);
					const bool bToRest = Step.To == Request.Goal || (Index + 1 < Chain.Num()
						&& IsSharpJoint(Step.Edge, Step.bReversed, Chain[Index + 1].Edge, Chain[Index + 1].bReversed));
					Step.Enter = Clock;
					Clock += Step.bFromRest ? (bToRest ? Seconds.RestRest : Seconds.RestRoll)
						: (bToRest ? Seconds.RollRest : Seconds.RollRoll);
					Step.Reach = Clock;
				}

				// What must be free, relative to the departure: every edge from entering it to reaching its far node,
				// every no-stop node as it is passed, the end node at least as it is reached - for ever at the goal.
				// The last two needs are the chain's last edge and its end node, which the next state is keyed on.
				TArray<FNeed> Needs;
				for (int32 Index = 0; Index < Chain.Num(); ++Index)
				{
					const FChainStep& Step = Chain[Index];
					const bool bEnd = Index == Chain.Num() - 1;
					Needs.Add({ FTaxiResource::Edge(Step.Edge), Step.Enter - Margin, Step.Reach + Margin });
					Needs.Add({ FTaxiResource::Node(Step.To), Step.Reach - Margin,
						bEnd && Step.To == Request.Goal ? FTaxiReservations::Forever : Step.Reach + Margin });
				}

				// THE EARLIEST DEPARTURE that fits every need at once: push it by the least shift any need asks, and
				// ask them all again, until a whole pass asks for none. Each push is forward and the intervals are
				// finite, so it settles; a rolling start cannot be pushed at all, and nothing leaves after LatestLeave.
				const double Latest = bRolling ? S.Arrive : S.LatestLeave;
				double Tau = bRolling ? S.Arrive : S.Arrive + StopLoss;
				bool bFits = Tau <= Latest;
				bool bSettled = false;
				int32 EndNodeInterval = 0;
				int32 EndEdgeInterval = 0;
				for (int32 Pass = 0; bFits && !bSettled && Pass < 64 * Needs.Num(); ++Pass)
				{
					bSettled = true;
					for (int32 Index = 0; Index < Needs.Num(); ++Index)
					{
						const FNeed& Need = Needs[Index];
						const double Hi = Need.To >= FTaxiReservations::Forever ? FTaxiReservations::Forever : Tau + Need.To;
						double Shift = 0.0;
						int32 IntervalIndex = 0;
						if (!LeastShift(FreeOf(Need.Resource), Tau + Need.From, Hi, Shift, IntervalIndex))
						{
							bFits = false;
							break;
						}
						if (Shift > 0.0)
						{
							Tau += Shift;
							bSettled = false;
							if (Tau > Latest)
							{
								bFits = false;
								break;
							}
						}
						if (Index == Needs.Num() - 2)
						{
							EndEdgeInterval = IntervalIndex;
						}
						else if (Index == Needs.Num() - 1)
						{
							EndNodeInterval = IntervalIndex;
						}
					}
				}
				if (!bFits || !bSettled)
				{
					continue;
				}

				const FChainStep& Last = Chain.Last();
				const TArray<FTaxiInterval> EndNodeFree = FreeOf(FTaxiResource::Node(Last.To));
				const TArray<FTaxiInterval> EndEdgeFree = FreeOf(FTaxiResource::Edge(Last.Edge));

				FSippState Next;
				Next.Node = Last.To;
				Next.NodeInterval = EndNodeInterval;
				Next.InEdge = Last.Edge;
				Next.bInReversed = Last.bReversed;
				Next.EdgeInterval = EndEdgeInterval;
				Next.bInFromRest = Last.bFromRest;
				Next.Arrive = Tau + Last.Reach;
				Next.Left = Tau;
				Next.Chain = Chain;
				Next.Parent = Top.Value;

				// It may stay as long as the node AND the edge its tail is still on stay free.
				Next.LatestLeave = FMath::Min(EndNodeFree[EndNodeInterval].End, EndEdgeFree[EndEdgeInterval].End) - Margin;

				const FSippKey NextKey{ Next.Node, Next.NodeInterval, Next.InEdge, Next.EdgeInterval };
				if (Closed.Contains(NextKey))
				{
					continue;
				}
				if (const double* Known = BestArrive.Find(NextKey); Known != nullptr && *Known <= Next.Arrive)
				{
					continue;
				}
				BestArrive.Add(NextKey, Next.Arrive);
				const double Estimate = Next.Arrive + Heuristic(Next.Node);
				const int32 NextIndex = States.Add(MoveTemp(Next));
				Open.HeapPush(TPair<double, int32>(Estimate, NextIndex), ByEstimate);
			}
		}
	}

	if (Reached == INDEX_NONE)
	{
		return Refuse();
	}

	// BACK FROM THE GOAL to the start, then forwards: steps, legs, holds and the windows to book.
	TArray<int32> Path;
	for (int32 At = Reached; At != INDEX_NONE; At = States[At].Parent)
	{
		Path.Add(At);
	}
	Algo::Reverse(Path);

	const int32 Holder = Request.Holder;
	auto AddPass = [&Out, Holder](const FTaxiResource& Resource, double From, double To)
	{
		// A ZERO-LENGTH PASS IS NO PASS: with no margin a node driven straight through is held for no time at all, and
		// a half-open window of no length overlaps nothing - BookWindow would refuse it rather than store it.
		if (From < To)
		{
			Out.Passes.Add({ Resource, { Holder, From, To } });
		}
	};

	TArray<FRouteStep> Steps;
	for (int32 Link = 1; Link < Path.Num(); ++Link)
	{
		const FSippState& From = States[Path[Link - 1]];
		const FSippState& To = States[Path[Link]];
		const double Tau = To.Left;
		// When the aircraft leaves To's node: the next move's departure - or never, at the goal.
		const double LeavesEnd = Link + 1 < Path.Num() ? States[Path[Link + 1]].Left : To.Arrive;

		// A HOLD at From when it set off later than it had to: later than DepartAt at the start, later than the stop
		// itself anywhere else. Stopping for a sharp corner and pulling away at once is a stop, not a wait.
		const double Ready = From.Arrive + (To.Chain[0].bFromRest ? StopLossAt(From) : 0.0);
		if (Tau > Ready + UE_KINDA_SMALL_NUMBER)
		{
			Out.Holds.Add({ From.Node, From.Arrive, Tau });
		}

		// The node it set off from: the start from DepartAt, any other from arriving.
		AddPass(FTaxiResource::Node(From.Node), Link == 1 ? Request.DepartAt : From.Arrive - Margin, Tau + Margin);

		for (int32 Index = 0; Index < To.Chain.Num(); ++Index)
		{
			const FChainStep& Step = To.Chain[Index];
			const bool bEnd = Index == To.Chain.Num() - 1;

			// The edge from entering it until the TAIL leaves it - for the chain's last edge, when the aircraft leaves
			// the node it may wait at.
			AddPass(FTaxiResource::Edge(Step.Edge), Tau + Step.Enter - Margin,
				(bEnd ? FMath::Max(Tau + Step.Reach, LeavesEnd) : Tau + Step.Reach) + Margin);
			if (!bEnd)
			{
				AddPass(FTaxiResource::Node(Step.To), Tau + Step.Reach - Margin, Tau + Step.Reach + Margin);
			}

			FRouteStep RouteStep;
			RouteStep.Edge = Step.Edge;
			RouteStep.To = Step.To;
			RouteStep.bReversed = Step.bReversed;
			if (const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge))
			{
				// CARRIED as RouteSearch carries it - see FRouteStep::bReverseLeg.
				RouteStep.bReverseLeg = Edge->bReverseLeg;
			}
			Steps.Add(RouteStep);
			Out.Legs.Add({ Step.Edge, Step.To, Tau + Step.Enter, Tau + Step.Reach });
		}

		if (Link + 1 == Path.Num())
		{
			// The goal, for ever: it stays there.
			AddPass(FTaxiResource::Node(To.Node), To.Arrive - Margin, FTaxiReservations::Forever);
		}
	}

	Out.Route = RouteSearch::PlanFromSteps(Network, Request.Start, MoveTemp(Steps));
	Out.Arrival = States[Reached].Arrive;
	Out.RouteResult = ERouteResult::Found;
	Out.Result = ETaxiPlanResult::Planned;
	return Out;
}
