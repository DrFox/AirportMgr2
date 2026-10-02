#include "Model/TaxiPlanner.h"

#include <cmath>

#include "AirsideLog.h"
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
	 *
	 * DOMINANCE STILL IGNORES MOMENTUM (review of #527, noted, not fixed): an earlier arrival at a key prunes a later
	 * one, but only the later can ROLL ON at its own later instant - the earlier one leaves later than that only from
	 * rest, a few seconds slower over the next edge. A downstream window that only a rolling departure at the later
	 * instant fits is then missed, and the planner refuses or waits longer than it had to. Keying on the arrival time
	 * itself would end dominance altogether; the loss is a few seconds' start-up on one edge, against a plan that is
	 * still valid - a pessimism, never a conflict.
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

		/**
		 * The end of the free time it may wait into: the lesser of its node's interval end and its tail's edge's. It must
		 * LEAVE with its node window's high bound (Hi(Tau, 0)) no later than this - asked in that form, never as
		 * "Tau <= LeaveBy - Margin", so the search and the booking compute the one bound one way (review of #527).
		 */
		double LeaveBy = 0.0;

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

	/**
	 * THE ONE ARITHMETIC FOR A WINDOW'S BOUNDS - the search's free-interval checks and the booked passes both call
	 * these, so the two can never disagree by an ulp on a boundary (review of #527: the search summed Tau + (Reach - M)
	 * and the booking (Tau + Reach) - M, and 6 of 200 fractional boundaries planned a pass BookPasses then refused).
	 * Offset is a move's step offset (Enter or Reach), or 0 for the moment of departure itself.
	 * ENFORCED BY: Airside.Model.TaxiPlan.TouchingBoundaryFractionalReach
	 */
	double WindowLo(double Tau, double Offset, double Margin)
	{
		return (Tau + Offset) - Margin;
	}

	double WindowHi(double Tau, double Offset, double Margin)
	{
		return (Tau + Offset) + Margin;
	}

	/**
	 * A resource a move needs free over [WindowLo(Tau, From), WindowHi(Tau, To)], Tau its departure - or from
	 * WindowLo on for ever when bForever (the goal, and the edge its tail stays on there).
	 */
	struct FNeed
	{
		FTaxiResource Resource;
		double From = 0.0;
		double To = 0.0;
		bool bForever = false;
	};

	/**
	 * The least Shift >= 0 such that [Lo + Shift, Hi + Shift] lies in one of Free's intervals, and which one; false
	 * when none can hold it. Free is sorted and disjoint, so the first interval that fits gives the least shift.
	 * Hi == Forever fits only an open-ended interval. The caller re-checks after shifting with WindowLo/WindowHi, so a
	 * shift that rounds short is caught and pushed again rather than trusted.
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

	/** Tau moved on by Shift - at least to the next representable double, so a shift that rounds to nothing cannot loop. */
	double PushedOn(double Tau, double Shift)
	{
		const double Pushed = Tau + Shift;
		return Pushed > Tau ? Pushed : std::nextafter(Tau, TNumericLimits<double>::Max());
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
	const int32 MaxChainEdges = FMath::Max(1, Rules.TaxiPlanMaxChainEdges);
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
		Start.LeaveBy = StartFree[StartInterval].End;
		Start.Left = Request.DepartAt;
		States.Add(MoveTemp(Start));
		Open.HeapPush(TPair<double, int32>(Request.DepartAt + Heuristic(Request.Start), 0), ByEstimate);
	}

	FRouteEdgeFilter Filter(Network, Query);

	// A CAP THAT CUT THE SEARCH IS SAID (review of #527): a refusal that came from a knob and not from
	// traffic must be findable in the log, or "the planner refused" reads as "the airport is full".
	bool bChainCapHit = false;
	bool bPassCapHit = false;

	// Every chain of edges from a node through no-stop nodes to the next node the aircraft may stop at, or the goal.
	// Depth-first, never revisiting a node within one chain, and NEVER BACK ALONG THE EDGE JUST DRIVEN (review of
	// #527): the filter admits a two-way edge reversed from its far end, and "back the way it came" is a 180 on a
	// centreline that books the edge twice in overlapping windows. The edges admitted are RouteSearch's (Filter).
	// MEMOISED per (node, arriving edge) - the two things a node's chains depend on - so a node reached in several of
	// its intervals walks once.
	TMap<TPair<FGuidelineNodeId, FGuidelineEdgeId>, TArray<TArray<FChainStep>>> ChainsFrom;
	TFunction<void(FGuidelineNodeId, FGuidelineEdgeId, TArray<FChainStep>&, TSet<FGuidelineNodeId>&, TArray<TArray<FChainStep>>&)> Walk;
	Walk = [&](FGuidelineNodeId From, FGuidelineEdgeId Arrived, TArray<FChainStep>& Path, TSet<FGuidelineNodeId>& Visited,
		TArray<TArray<FChainStep>>& Into)
	{
		TArray<FAdmittedEdge> Leaving;
		Filter.ForEachAdmitted(From, [&Leaving](const FAdmittedEdge& Admitted) { Leaving.Add(Admitted); });
		for (const FAdmittedEdge& Admitted : Leaving)
		{
			if (Admitted.Id == Arrived || Visited.Contains(Admitted.Next) || !SecondsFor(Admitted.Id, Admitted.bReversed).bUsable)
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
				Into.Add(Path);
			}
			else if (Path.Num() < MaxChainEdges)
			{
				Visited.Add(Admitted.Next);
				Walk(Admitted.Next, Admitted.Id, Path, Visited, Into);
				Visited.Remove(Admitted.Next);
			}
			else
			{
				bChainCapHit = true;
			}
			Path.Pop();
		}
	};
	auto ChainsOf = [&](FGuidelineNodeId Node, FGuidelineEdgeId Arrived) -> const TArray<TArray<FChainStep>>&
	{
		const TPair<FGuidelineNodeId, FGuidelineEdgeId> Key(Node, Arrived);
		if (const TArray<TArray<FChainStep>>* Known = ChainsFrom.Find(Key))
		{
			return *Known;
		}
		TArray<TArray<FChainStep>> Fresh;
		TArray<FChainStep> Path;
		TSet<FGuidelineNodeId> Visited;
		Visited.Add(Node);
		Walk(Node, Arrived, Path, Visited, Fresh);
		return ChainsFrom.Add(Key, MoveTemp(Fresh));
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

		// COPIED: the memo's chains are shared by every state at this node, and each variant below writes its own
		// offsets into the steps.
		const TArray<TArray<FChainStep>> Chains = ChainsOf(S.Node, S.InEdge);
		for (const TArray<FChainStep>& Walked : Chains)
		{
			// TWO WAYS TO SET OFF: rolling straight on (not from the start, nor round an instant corner), or from rest
			// after a stop here - waiting as long as the node and the tail's edge allow.
			for (int32 Variant = 0; Variant < 2; ++Variant)
			{
				const bool bRolling = Variant == 0;
				if (bRolling && (bAtStart || IsSharpJoint(S.InEdge, S.bInReversed, Walked[0].Edge, Walked[0].bReversed)))
				{
					continue;
				}

				// Offsets along the chain from the departure: each step's duration from the authority's four timings -
				// at rest after a stop or a sharp joint, at rest into a sharp joint or the goal.
				TArray<FChainStep> Chain = Walked;
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
				// every no-stop node as it is passed, the end node at least as it is reached. AT THE GOAL BOTH FOR EVER:
				// the node, and the edge the tail stays on (review of #527: the tail sat on an edge released at Reach + M).
				// The last two needs are the chain's last edge and its end node, which the next state is keyed on.
				TArray<FNeed> Needs;
				for (int32 Index = 0; Index < Chain.Num(); ++Index)
				{
					const FChainStep& Step = Chain[Index];
					const bool bAtGoal = Index == Chain.Num() - 1 && Step.To == Request.Goal;
					Needs.Add({ FTaxiResource::Edge(Step.Edge), Step.Enter, Step.Reach, bAtGoal });
					Needs.Add({ FTaxiResource::Node(Step.To), Step.Reach, Step.Reach, bAtGoal });
				}

				// THE EARLIEST DEPARTURE NOT BEFORE Earliest that fits every need at once: push it by the least shift any
				// need asks, and ask them all again, until a whole pass asks for none. Each push is forward and the
				// intervals are finite, so it settles; a rolling start cannot be pushed at all, and nothing leaves with
				// its node window ending past LeaveBy. Writes the end edge's and end node's interval indices.
				auto Settle = [&](double Earliest, double& OutTau, int32& OutEndEdge, int32& OutEndNode)
				{
					double Tau = Earliest;
					if (WindowHi(Tau, 0.0, Margin) > S.LeaveBy)
					{
						return false;
					}
					const int32 MaxPasses = 64 * Needs.Num();
					for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
					{
						bool bSettled = true;
						for (int32 Index = 0; Index < Needs.Num(); ++Index)
						{
							const FNeed& Need = Needs[Index];
							const double Lo = WindowLo(Tau, Need.From, Margin);
							const double Hi = Need.bForever ? FTaxiReservations::Forever : WindowHi(Tau, Need.To, Margin);
							double Shift = 0.0;
							int32 IntervalIndex = 0;
							if (!LeastShift(FreeOf(Need.Resource), Lo, Hi, Shift, IntervalIndex))
							{
								return false;
							}
							if (Shift > 0.0)
							{
								if (bRolling)
								{
									return false;
								}
								Tau = PushedOn(Tau, Shift);
								bSettled = false;
								if (WindowHi(Tau, 0.0, Margin) > S.LeaveBy)
								{
									return false;
								}
							}
							if (Index == Needs.Num() - 2)
							{
								OutEndEdge = IntervalIndex;
							}
							else if (Index == Needs.Num() - 1)
							{
								OutEndNode = IntervalIndex;
							}
						}
						if (bSettled)
						{
							OutTau = Tau;
							return true;
						}
					}
					bPassCapHit = true;
					return false;
				};

				const FChainStep& Last = Chain.Last();
				const TArray<FTaxiInterval> EndNodeFree = FreeOf(FTaxiResource::Node(Last.To));
				const TArray<FTaxiInterval> EndEdgeFree = FreeOf(FTaxiResource::Edge(Last.Edge));

				// ONE SUCCESSOR PER REACHABLE FREE INTERVAL of where the move ends (review of #527) - textbook SIPP's
				// rule, which trying only the earliest departure broke: reaching a node early in an interval that closes
				// before the way on opens is a dead end, and the arrival in its NEXT interval (waiting here first) is the
				// plan. So after each fit, the departure is pushed to the first instant the end node or the end edge
				// could fall in a later interval, and settled again. A rolling start has one instant and one successor.
				double Earliest = bRolling ? S.Arrive : S.Arrive + StopLoss;
				for (int32 Successor = 0; Successor < 64; ++Successor)
				{
					double Tau = 0.0;
					int32 EndNodeInterval = 0;
					int32 EndEdgeInterval = 0;
					if (!Settle(Earliest, Tau, EndEdgeInterval, EndNodeInterval))
					{
						break;
					}

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
					Next.LeaveBy = FMath::Min(EndNodeFree[EndNodeInterval].End, EndEdgeFree[EndEdgeInterval].End);

					const FSippKey NextKey{ Next.Node, Next.NodeInterval, Next.InEdge, Next.EdgeInterval };
					const double* Known = BestArrive.Find(NextKey);
					if (!Closed.Contains(NextKey) && (Known == nullptr || *Known > Next.Arrive))
					{
						BestArrive.Add(NextKey, Next.Arrive);
						const double Estimate = Next.Arrive + Heuristic(Next.Node);
						const int32 NextIndex = States.Add(MoveTemp(Next));
						Open.HeapPush(TPair<double, int32>(Estimate, NextIndex), ByEstimate);
					}

					if (bRolling)
					{
						break;
					}
					// The first departure that could put the end node or the end edge in a LATER interval than this one.
					double Later = TNumericLimits<double>::Max();
					if (EndNodeFree.IsValidIndex(EndNodeInterval + 1))
					{
						Later = FMath::Min(Later, EndNodeFree[EndNodeInterval + 1].Start + Margin - Last.Reach);
					}
					if (EndEdgeFree.IsValidIndex(EndEdgeInterval + 1))
					{
						Later = FMath::Min(Later, EndEdgeFree[EndEdgeInterval + 1].Start + Margin - Last.Enter);
					}
					if (Later >= TNumericLimits<double>::Max())
					{
						break;
					}
					Earliest = FMath::Max(Later, PushedOn(Tau, 0.0));
				}
			}
		}
	}

	// ONCE PER SESSION, not per plan: on a dense layout a cap can bite on every request, and the line is evidence that a
	// knob shapes plans, not a per-request event.
	static bool bWarnedOfCap = false;
	if ((bChainCapHit || bPassCapHit) && !bWarnedOfCap)
	{
		bWarnedOfCap = true;
		UE_LOG(LogAirsideTaxiPlan, Warning,
			TEXT("TaxiPlan: agent %d's search was cut short by a cap (%s%s) - a refusal or a later plan may be the cap's, not traffic's"),
			Request.Holder, bChainCapHit ? TEXT("a junction chain longer than TaxiPlanMaxChainEdges") : TEXT(""),
			bPassCapHit ? TEXT(" a departure that did not settle in 64 passes per need") : TEXT(""));
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

	// EVERY BOUND BELOW IS WindowLo/WindowHi OF THE SAME (Tau, offset) THE SEARCH CHECKED - see WindowLo.
	TArray<FRouteStep> Steps;
	for (int32 Link = 1; Link < Path.Num(); ++Link)
	{
		const FSippState& From = States[Path[Link - 1]];
		const FSippState& To = States[Path[Link]];
		const double Tau = To.Left;
		// When the aircraft leaves To's node: the next move's departure - or never, at the goal.
		const bool bLast = Link + 1 == Path.Num();
		const double LeavesEnd = bLast ? To.Arrive : States[Path[Link + 1]].Left;

		// A HOLD at From when it set off later than it had to: later than DepartAt at the start, later than the stop
		// itself anywhere else. Stopping for a sharp corner and pulling away at once is a stop, not a wait.
		const double Ready = From.Arrive + (To.Chain[0].bFromRest ? StopLossAt(From) : 0.0);
		if (Tau > Ready + UE_KINDA_SMALL_NUMBER)
		{
			Out.Holds.Add({ From.Node, From.Arrive, Tau });
		}

		// The node it set off from: the start from DepartAt, any other from arriving (the search's end-node need of the
		// move that brought it there).
		const double FromLo = Link == 1 ? Request.DepartAt : WindowLo(From.Left, From.Chain.Last().Reach, Margin);
		AddPass(FTaxiResource::Node(From.Node), FromLo, WindowHi(Tau, 0.0, Margin));

		for (int32 Index = 0; Index < To.Chain.Num(); ++Index)
		{
			const FChainStep& Step = To.Chain[Index];
			const bool bEnd = Index == To.Chain.Num() - 1;

			// The edge from entering it until the TAIL leaves it - for the chain's last edge, when the aircraft leaves
			// the node it may wait at; at the goal, never.
			const double EdgeHi = !bEnd ? WindowHi(Tau, Step.Reach, Margin)
				: bLast ? FTaxiReservations::Forever
				: FMath::Max(WindowHi(Tau, Step.Reach, Margin), WindowHi(LeavesEnd, 0.0, Margin));
			AddPass(FTaxiResource::Edge(Step.Edge), WindowLo(Tau, Step.Enter, Margin), EdgeHi);
			if (!bEnd)
			{
				AddPass(FTaxiResource::Node(Step.To), WindowLo(Tau, Step.Reach, Margin), WindowHi(Tau, Step.Reach, Margin));
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

		if (bLast)
		{
			// The goal, for ever: it stays there.
			AddPass(FTaxiResource::Node(To.Node), WindowLo(Tau, To.Chain.Last().Reach, Margin), FTaxiReservations::Forever);
		}
	}

	Out.Route = RouteSearch::PlanFromSteps(Network, Request.Start, MoveTemp(Steps));
	Out.Arrival = States[Reached].Arrive;
	Out.RouteResult = ERouteResult::Found;
	Out.Result = ETaxiPlanResult::Planned;
	return Out;
}
