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

		/** Which way it holds the edge: the way it drives, or Any on push ground (a push and the taxi back over it). */
		ETaxiWay Way = ETaxiWay::Any;
	};

	/**
	 * One SIPP search state - ALWAYS AT A NODE THE AIRCRAFT MAY STOP AT (the start, a CanHoldAt node, the end of a
	 * push, or the goal), in one PLACE of its order, having arrived along InEdge in one place of THAT edge's order.
	 *
	 * WHY ONLY THERE, and why the search moves in CHAINS between such nodes: textbook SIPP (Phillips & Likhachev
	 * 2011) keys a state on (node, interval) and lets the earliest arrival dominate, because an aircraft that
	 * arrives early can always wait. Inside a junction it may not wait, so an early arrival at a junction node does
	 * NOT dominate a later one - measured 2026-10-02 on WaitsAtPlainNodeNotInJunction: the early arrival at J1 was a
	 * dead end (its way out was booked) and, keyed alike, it discarded the later arrival that would have fitted. So
	 * a move runs from a stoppable node through every no-stop node to the next stoppable one as ONE step, with ONE
	 * departure time that fits the whole chain - and states exist only where waiting, and so dominance, holds.
	 *
	 * KEYED ON A PLACE IN THE ORDER, NOT A FREE INTERVAL (PR 2): once an edge may carry several aircraft the same way
	 * (FTaxiReservations::MayShare), its free time is no longer a list of gaps. A window's place - how many windows
	 * start before it (PlaceAt) - is what an interval index was: two arrivals in one place have the same aircraft
	 * ahead and behind, and so the same freedom to wait.
	 *
	 * THE ARRIVING EDGE AND ITS PLACE ARE IN THE KEY, which textbook SIPP's are not: while the aircraft waits it
	 * still holds that edge with its tail, so how long it may wait is the edge's as much as the node's.
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
		int32 NodePlace = 0;
		FGuidelineEdgeId InEdge;
		bool bInReversed = false;
		int32 EdgePlace = 0;

		/** Whether InEdge was entered from rest - which of its timings a stop here is measured from. */
		bool bInFromRest = false;

		/**
		 * At rest here already, with no momentum to keep and no stop to make: the start, and the end of a push. A
		 * state at rest neither rolls on nor pays a stop's cost.
		 */
		bool bAtRest = false;

		/**
		 * The edge it may NOT drive straight back along: the one it arrived on (no 180 on a centreline - review of #527),
		 * unset at the start and at the end of a push, which reverses by design: the taxi out leaves along the arm the
		 * push backed onto.
		 */
		FGuidelineEdgeId NoReverseOf;

		/** When it reaches Node - rolling, or at rest when it is the start. */
		double Arrive = 0.0;

		/**
		 * The end of the time it may wait into: the lesser of how long its node's window and its tail's edge's window may
		 * stretch (FTaxiReservations::LatestEnd). It must LEAVE with its node window's high bound (WindowHi(Tau, 0)) no
		 * later than this - asked in that form, never as "Tau <= LeaveBy - Margin", so the search and the booking compute
		 * the one bound one way (review of #527).
		 */
		double LeaveBy = 0.0;

		/** When the move that brought it here set off from the previous state's node. */
		double Left = 0.0;

		/** That move's edges. */
		TArray<FChainStep> Chain;

		/** That move was the push off the stand. */
		bool bPushMove = false;

		/** Planning ALONG a route (FTaxiRequest::Along): the index of the next step to drive. 0 otherwise. */
		int32 AlongAt = 0;

		int32 Parent = INDEX_NONE;
	};

	struct FSippKey
	{
		FGuidelineNodeId Node;
		int32 NodePlace = 0;
		FGuidelineEdgeId InEdge;
		int32 EdgePlace = 0;

		/** Along a route, how far along it: a loop's node met twice is two states, not one. */
		int32 AlongAt = 0;

		bool operator==(const FSippKey& Other) const
		{
			return Node == Other.Node && NodePlace == Other.NodePlace && InEdge == Other.InEdge
				&& EdgePlace == Other.EdgePlace && AlongAt == Other.AlongAt;
		}
	};

	uint32 GetTypeHash(const FSippKey& Key)
	{
		return HashCombine(HashCombine(HashCombine(GetTypeHash(Key.Node), ::GetTypeHash(Key.NodePlace)),
			HashCombine(GetTypeHash(Key.InEdge), ::GetTypeHash(Key.EdgePlace))), ::GetTypeHash(Key.AlongAt));
	}

	bool ByEstimate(const TPair<double, int32>& A, const TPair<double, int32>& B)
	{
		return A.Key < B.Key;
	}

	/**
	 * THE ONE ARITHMETIC FOR A WINDOW'S BOUNDS - the search's checks and the booked passes both call these, so the two
	 * can never disagree by an ulp on a boundary (review of #527: the search summed Tau + (Reach - M) and the booking
	 * (Tau + Reach) - M, and 6 of 200 fractional boundaries planned a pass BookPasses then refused). Offset is a move's
	 * step offset (Enter or Reach), or 0 for the moment of departure itself.
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
	 * A resource a move needs over [WindowLo(Tau, From), WindowHi(Tau, To)] going Way, Tau its departure - or from
	 * WindowLo on for ever when bForever (the goal, and the edge its tail stays on there).
	 */
	struct FNeed
	{
		FTaxiResource Resource;
		double From = 0.0;
		double To = 0.0;
		bool bForever = false;
		ETaxiWay Way = ETaxiWay::Any;
	};

	/** Tau moved on by Shift - at least to the next representable double, so a shift that rounds to nothing cannot loop. */
	double PushedOn(double Tau, double Shift)
	{
		const double Pushed = Tau + Shift;
		return Pushed > Tau ? Pushed : std::nextafter(Tau, TNumericLimits<double>::Max());
	}

	/** Whether a move books (and the order asks for) its END node from its start - a push, or a move of more than one edge. */
	bool MoveHoldsEndFromStart(bool bPushMove, int32 Edges)
	{
		return bPushMove || Edges > 1;
	}

	/** The way a step drives its edge: A to B unless walked reversed. */
	ETaxiWay WayOf(bool bReversed)
	{
		return bReversed ? ETaxiWay::BToA : ETaxiWay::AToB;
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

FTaxiPlan FTaxiPlanner::Plan(const FTaxiRequest& Request)
{
	FTaxiPlan Out;

	// Where the TAXI starts: the stand, or where a push ends - the push's own route is PushbackPlanner's, already found.
	const bool bPush = Request.PushSteps.Num() > 0;
	const FGuidelineNodeId TaxiStart = bPush ? Request.PushSteps.Last().To : Request.Start;

	// RouteSearch's query, built by its one factory, so the policy (runway avoidance) and the size gates are the
	// ones a RouteSearch::Find for this errand would apply.
	const FRouteQuery Query = FRouteQuery::For(Request.Errand, TaxiStart, Request.Goal, Airframe.Wingspan,
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

	// ALONG A ROUTE (PR 3): its steps are the only edges, its end the goal, counted by steps. It may start and end on one
	// node (a loop), so the start-is-goal refusal is the free search's only.
	const bool bAlong = Request.Along.Num() > 0 && !bPush;
	if ((!bAlong && TaxiStart == Request.Goal) || Network.GetGuidelineNode(Request.Start) == nullptr
		|| Network.GetGuidelineNode(Request.Goal) == nullptr || Network.GetGuidelineNode(TaxiStart) == nullptr)
	{
		return Refuse();
	}

	const double Margin = FMath::Max(0.0, Rules.TaxiPlanMargin);
	const int32 MaxChainEdges = FMath::Max(1, Rules.TaxiPlanMaxChainEdges);
	const FVector2D GoalAt = Network.GetGuidelineNode(Request.Goal)->Position;
	const double TopSpeed = Airframe.Chassis.Ground.Taxi.SpeedCap;
	const int32 Holder = Request.Holder;

	// The push's edges: held both ways (ETaxiWay::Any) by the push, so the taxi back over one is held both ways too -
	// the two merge into one window when booked, and a window that is both ways shares with nothing.
	TSet<FGuidelineEdgeId> PushEdges;
	for (const FRouteStep& Step : Request.PushSteps)
	{
		PushEdges.Add(Step.Edge);
	}
	auto WayFor = [&PushEdges](FGuidelineEdgeId Edge, bool bReversed)
	{
		return PushEdges.Contains(Edge) ? ETaxiWay::Any : WayOf(bReversed);
	};

	// Straight-line distance at the taxi cap: never more than the real time, because no piece is driven faster than
	// the cap (FSpeedProfile caps every span at it). So the first goal popped is the earliest arrival.
	auto Heuristic = [this, &GoalAt, TopSpeed](FGuidelineNodeId Node)
	{
		const FGuidelineNode* At = Network.GetGuidelineNode(Node);
		return (At != nullptr && TopSpeed > 0.0) ? FVector2D::Distance(At->Position, GoalAt) / TopSpeed : 0.0;
	};

	// The stop's own cost at a state: driving its arriving edge to rest instead of rolling off it - the brake, on the
	// edge just driven. A state at rest (the start, a push's end) has none.
	auto StopLossAt = [this](const FSippState& State)
	{
		if (State.bAtRest || !State.InEdge.IsSet())
		{
			return 0.0;
		}
		const FTaxiEdgeSeconds& In = SecondsFor(State.InEdge, State.bInReversed);
		return FMath::Max(0.0, State.bInFromRest ? In.RestRest - In.RestRoll : In.RollRest - In.RollRoll);
	};

	// THE START: on Start from DepartAt. Somebody else holding the node it is standing on means the table disagrees
	// with the world, and the honest answer is no plan rather than one that books over them.
	const FTaxiResource StartNode = FTaxiResource::Node(Request.Start);
	double StartShift = 0.0;
	if (!Table.EarliestFit(StartNode, ETaxiWay::Any, Request.DepartAt, PushedOn(Request.DepartAt, 0.0), Holder, StartShift)
		|| StartShift > 0.0)
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
		Start.NodePlace = Table.PlaceAt(StartNode, Request.DepartAt, Holder);
		Start.bAtRest = !Request.bStartsRolling;
		Start.Arrive = Request.DepartAt;
		Start.Left = Request.DepartAt;
		Start.LeaveBy = Table.LatestEnd(StartNode, ETaxiWay::Any, Request.DepartAt, Holder);
		if (!Request.bMayWaitAtStart)
		{
			// LEAVES AT DepartAt OR NOT AT ALL: an arrival's start is the runway exit, and a wait there is a wait on the runway.
			Start.LeaveBy = FMath::Min(Start.LeaveBy, WindowHi(Request.DepartAt, 0.0, Margin));
		}
		States.Add(MoveTemp(Start));
		Open.HeapPush(TPair<double, int32>(Request.DepartAt + Heuristic(Request.Start), 0), ByEstimate);
	}

	FRouteEdgeFilter Filter(Network, Query);

	// A CAP THAT CUT THE SEARCH IS SAID (review of #527): a refusal that came from a knob and not from traffic must be
	// findable in the log, or "the planner refused" reads as "the airport is full".
	bool bChainCapHit = false;
	bool bPassCapHit = false;

	// Every chain of edges from a node through no-stop nodes to the next node the aircraft may stop at, or the goal.
	// Depth-first, never revisiting a node within one chain, and NEVER BACK ALONG THE EDGE JUST DRIVEN (review of
	// #527): the filter admits a two-way edge reversed from its far end, and "back the way it came" is a 180 on a
	// centreline that books the edge twice in overlapping windows. The edges admitted are RouteSearch's (Filter).
	// MEMOISED per (node, edge it may not reverse along) - the two things a node's chains depend on.
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
			Step.Way = WayFor(Admitted.Id, Admitted.bReversed);
			Path.Add(Step);
			if (Admitted.Next == Request.Goal || CanHoldAt(Network, Rules, Admitted.Id, Admitted.Next, &Reach))
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
	auto ChainsOf = [&](FGuidelineNodeId Node, FGuidelineEdgeId NoReverse) -> const TArray<TArray<FChainStep>>&
	{
		const TPair<FGuidelineNodeId, FGuidelineEdgeId> Key(Node, NoReverse);
		if (const TArray<TArray<FChainStep>>* Known = ChainsFrom.Find(Key))
		{
			return *Known;
		}
		TArray<TArray<FChainStep>> Fresh;
		TArray<FChainStep> Path;
		TSet<FGuidelineNodeId> Visited;
		Visited.Add(Node);
		Walk(Node, NoReverse, Path, Visited, Fresh);
		return ChainsFrom.Add(Key, MoveTemp(Fresh));
	};

	// THE PUSH, as the one move out of the start: its route is PushbackPlanner's, fixed, timed whole (PushSeconds),
	// and held both ways from the moment it starts to the moment it ends - every edge and every node of it, the end
	// node included. Enter 0 and Reach PushSeconds on every step say exactly that to the needs below.
	TArray<FChainStep> PushChain;
	for (const FRouteStep& Step : Request.PushSteps)
	{
		FChainStep Pushed;
		Pushed.Edge = Step.Edge;
		Pushed.bReversed = Step.bReversed;
		Pushed.To = Step.To;
		Pushed.Enter = 0.0;
		Pushed.Reach = FMath::Max(0.0, Request.PushSeconds);
		Pushed.bFromRest = true;
		Pushed.Way = ETaxiWay::Any;
		PushChain.Add(Pushed);
	}

	int32 Reached = INDEX_NONE;
	while (Open.Num() > 0)
	{
		TPair<double, int32> Top;
		Open.HeapPop(Top, ByEstimate);
		const FSippState S = States[Top.Value];
		const FSippKey Key{ S.Node, S.NodePlace, S.InEdge, S.EdgePlace, S.AlongAt };
		if (Closed.Contains(Key))
		{
			continue;
		}
		Closed.Add(Key);

		if (bAlong ? S.AlongAt >= Request.Along.Num() : S.Node == Request.Goal)
		{
			Reached = Top.Value;
			break;
		}

		const bool bAtStart = Top.Value == 0;
		const bool bPushMove = bPush && bAtStart;
		const double StopLoss = StopLossAt(S);

		// COPIED: the memo's chains are shared by every state at this node, and each variant below writes its own
		// offsets into the steps.
		TArray<TArray<FChainStep>> Chains;
		if (bPushMove)
		{
			Chains.Add(PushChain);
		}
		else if (bAlong)
		{
			// THE ONE CHAIN THE ROUTE ALLOWS: its next steps, to the next node it may stop at or its end - the moves the free
			// search would make along the same edges, so the order sees the same moves either way.
			TArray<FChainStep> Next;
			bool bUsable = true;
			for (int32 Index = S.AlongAt; Index < Request.Along.Num() && bUsable; ++Index)
			{
				const FRouteStep& Step = Request.Along[Index];
				bUsable = SecondsFor(Step.Edge, Step.bReversed).bUsable;
				FChainStep Chained;
				Chained.Edge = Step.Edge;
				Chained.bReversed = Step.bReversed;
				Chained.To = Step.To;
				Chained.Way = WayFor(Step.Edge, Step.bReversed);
				Next.Add(Chained);
				if (Index + 1 == Request.Along.Num() || CanHoldAt(Network, Rules, Step.Edge, Step.To, &Reach))
				{
					break;
				}
			}
			if (bUsable && Next.Num() > 0)
			{
				Chains.Add(MoveTemp(Next));
			}
		}
		else
		{
			Chains = ChainsOf(S.Node, S.NoReverseOf);
		}

		for (const TArray<FChainStep>& Walked : Chains)
		{
			// TWO WAYS TO SET OFF: rolling straight on (not from rest, nor round an instant corner), or from rest after a
			// stop here - waiting as long as the node and the tail's edge allow. A push is from rest, timed whole.
			// Whether this chain ends at the goal: the route's last step when planning along one, the goal node otherwise.
			const bool bChainAtGoal = bAlong ? S.AlongAt + Walked.Num() >= Request.Along.Num() : Walked.Last().To == Request.Goal;
			for (int32 Variant = 0; Variant < 2; ++Variant)
			{
				const bool bRolling = Variant == 0;
				if (bRolling && (S.bAtRest || bPushMove || IsSharpJoint(S.InEdge, S.bInReversed, Walked[0].Edge, Walked[0].bReversed)))
				{
					continue;
				}

				// Offsets along the chain from the departure: each step's duration from the authority's four timings -
				// at rest after a stop or a sharp joint, at rest into a sharp joint or the goal. A push's are fixed.
				TArray<FChainStep> Chain = Walked;
				if (!bPushMove)
				{
					double Clock = 0.0;
					for (int32 Index = 0; Index < Chain.Num(); ++Index)
					{
						FChainStep& Step = Chain[Index];
						const FTaxiEdgeSeconds& Seconds = SecondsFor(Step.Edge, Step.bReversed);
						Step.bFromRest = Index == 0 ? !bRolling
							: IsSharpJoint(Chain[Index - 1].Edge, Chain[Index - 1].bReversed, Step.Edge, Step.bReversed);
						const bool bToRest = (Index + 1 == Chain.Num() && bChainAtGoal) || (Index + 1 < Chain.Num()
							&& IsSharpJoint(Step.Edge, Step.bReversed, Chain[Index + 1].Edge, Chain[Index + 1].bReversed));
						Step.Enter = Clock;
						Clock += Step.bFromRest ? (bToRest ? Seconds.RestRest : Seconds.RestRoll)
							: (bToRest ? Seconds.RollRest : Seconds.RollRoll);
						Step.Reach = Clock;
					}
				}

				// What must be held, relative to the departure: every edge and every node PASSED from the MOVE'S START (offset
				// 0) until it is left; the end node from being reached. AT THE GOAL BOTH FOR EVER: the node, and the edge the
				// tail stays on. The last two needs are the chain's last edge and its end node, which the next state is keyed on.
				//
				// FROM THE MOVE'S START, NOT FROM ENTERING EACH (measured on M_ScaleGatwick, 2026-10-02): the order lets an
				// aircraft into a move all or nothing (UTaxiPlanning::OrderHold - never held inside a junction), so every
				// resource of a move must be booked as the move is - from when it sets off. Booked from entering each, two
				// moves through one junction were ordered A-then-B on one node and B-then-A on another, each was let into its
				// move only once the other had gone, and they waited on each other for good. With every window of a move
				// starting with it, a wait always points at a move booked to start EARLIER - which cannot be a cycle. A lane's
				// one-edge move books exactly what it did: its edge is entered at the move's start.
				//
				// A MOVE THROUGH A JUNCTION (more than one edge) BOOKS ITS END NODE FROM ITS START TOO, and the order asks for
				// it with the rest (measured, the same run: two moves through one junction interleaved - one passed the far
				// node before the other set off and reached its own end after it - and the first, early at its end and still
				// claiming the far node behind it, waited on the second, which waited on that node). A lane's one-edge move
				// books its end node from reaching it: the lane is long enough to wait on, clear of where it came from.
				const bool bWholeMove = MoveHoldsEndFromStart(bPushMove, Chain.Num());
				TArray<FNeed> Needs;
				for (int32 Index = 0; Index < Chain.Num(); ++Index)
				{
					const FChainStep& Step = Chain[Index];
					const bool bEnd = Index == Chain.Num() - 1;
					const bool bAtGoal = bEnd && bChainAtGoal;
					Needs.Add({ FTaxiResource::Edge(Step.Edge), 0.0, Step.Reach, bAtGoal, Step.Way });
					Needs.Add({ FTaxiResource::Node(Step.To), (bWholeMove || !bEnd) ? 0.0 : Step.Reach, Step.Reach, bAtGoal, ETaxiWay::Any });
				}

				// THE EARLIEST DEPARTURE NOT BEFORE Earliest that every need shares with: push it by the least shift any need
				// asks (FTaxiReservations::EarliestFit), and ask them all again, until a whole pass asks for none. Each push is
				// forward and resolves a window for good, so it settles; a rolling start cannot be pushed at all, and nothing
				// leaves with its node window ending past LeaveBy.
				auto Settle = [&](double Earliest, double& OutTau)
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
						for (const FNeed& Need : Needs)
						{
							const double Lo = WindowLo(Tau, Need.From, Margin);
							const double Hi = Need.bForever ? FTaxiReservations::Forever : WindowHi(Tau, Need.To, Margin);
							double Shift = 0.0;
							if (!Table.EarliestFit(Need.Resource, Need.Way, Lo, Hi, Holder, Shift))
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
				const FTaxiResource EndNode = FTaxiResource::Node(Last.To);
				const FTaxiResource EndEdge = FTaxiResource::Edge(Last.Edge);
				const double EndNodeOffset = bWholeMove ? 0.0 : Last.Reach;

				// ONE SUCCESSOR PER REACHABLE PLACE of where the move ends (review of #527) - textbook SIPP's one successor
				// per safe interval: reaching a node early, in a place whose time runs out before the way on opens, is a
				// dead end, and the arrival in a LATER place (waiting here first) is the plan. So after each fit the
				// departure is pushed to the first instant the end node or the end edge could take a later place, and
				// settled again. A rolling start has one instant and one successor.
				double Earliest = bRolling ? S.Arrive : S.Arrive + StopLoss;
				for (int32 Successor = 0; Successor < 64; ++Successor)
				{
					double Tau = 0.0;
					if (!Settle(Earliest, Tau))
					{
						break;
					}
					const double NodeLo = WindowLo(Tau, EndNodeOffset, Margin);
					const double EdgeLo = WindowLo(Tau, 0.0, Margin);

					FSippState Next;
					Next.Node = Last.To;
					Next.NodePlace = Table.PlaceAt(EndNode, NodeLo, Holder);
					Next.InEdge = Last.Edge;
					Next.bInReversed = Last.bReversed;
					Next.EdgePlace = Table.PlaceAt(EndEdge, EdgeLo, Holder);
					Next.bInFromRest = Last.bFromRest;
					Next.bAtRest = bPushMove;
					Next.NoReverseOf = bPushMove ? FGuidelineEdgeId() : Last.Edge;
					Next.Arrive = Tau + Last.Reach;
					Next.Left = Tau;
					Next.Chain = Chain;
					Next.bPushMove = bPushMove;
					Next.AlongAt = bAlong ? S.AlongAt + Chain.Num() : 0;
					Next.Parent = Top.Value;

					// It may stay as long as the node AND the edge its tail is still on may stretch.
					Next.LeaveBy = FMath::Min(Table.LatestEnd(EndNode, ETaxiWay::Any, NodeLo, Holder),
						Table.LatestEnd(EndEdge, Last.Way, EdgeLo, Holder));

					const FSippKey NextKey{ Next.Node, Next.NodePlace, Next.InEdge, Next.EdgePlace, Next.AlongAt };
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
					// The first departure that could put the end node or the end edge in a LATER place than this one.
					const double NodeNext = Table.NextPlaceAfter(EndNode, ETaxiWay::Any, NodeLo, Holder);
					const double EdgeNext = Table.NextPlaceAfter(EndEdge, Last.Way, EdgeLo, Holder);
					double Later = TNumericLimits<double>::Max();
					if (NodeNext < FTaxiReservations::Forever)
					{
						Later = FMath::Min(Later, NodeNext + Margin - EndNodeOffset);
					}
					if (EdgeNext < FTaxiReservations::Forever)
					{
						Later = FMath::Min(Later, EdgeNext + Margin);
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

	// ONE WINDOW PER RESOURCE PER PLAN: a pass on a resource this plan already holds - the push's last edge, driven back
	// along by the taxi - joins the window it touches, both ways if the two differ. BookPasses would refuse the pair:
	// two windows of one holder that may not share have no order either.
	// THE PUSH'S OWN WINDOWS (the first link's) that no later pass joined: released when the push hands over to the taxi
	// (UTaxiPlanning::Track) - a push backs over ground the taxi may drive again later, in a window of its own.
	int32 PushPasses = 0;
	TSet<int32> PushJoined;
	auto AddPass = [&Out, Holder, &PushPasses, &PushJoined](const FTaxiResource& Resource, double From, double To, ETaxiWay Way)
	{
		// A ZERO-LENGTH PASS IS NO PASS: with no margin a node driven straight through is held for no time at all, and
		// a half-open window of no length overlaps nothing - BookWindow would refuse it rather than store it.
		if (!(From < To))
		{
			return;
		}
		for (int32 Index = 0; Index < Out.Passes.Num(); ++Index)
		{
			FTaxiPass& Pass = Out.Passes[Index];
			if (Pass.Resource == Resource && Pass.Window.From <= To && From <= Pass.Window.To)
			{
				if (Index < PushPasses)
				{
					PushJoined.Add(Index);
				}
				Pass.Window.From = FMath::Min(Pass.Window.From, From);
				Pass.Window.To = FMath::Max(Pass.Window.To, To);
				Pass.Window.Way = Pass.Window.Way == Way ? Way : ETaxiWay::Any;
				return;
			}
		}
		Out.Passes.Add({ Resource, { Holder, From, To, Way } });
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
		if (Link == 1)
		{
			Out.PushAt = Tau;
		}

		// The node it set off from: the start from DepartAt, any other from arriving (the search's end-node need of the
		// move that brought it there - from the push's start, for the node a push ended on).
		const double FromLo = Link == 1 ? Request.DepartAt
			: WindowLo(From.Left, MoveHoldsEndFromStart(From.bPushMove, From.Chain.Num()) ? 0.0 : From.Chain.Last().Reach, Margin);
		AddPass(FTaxiResource::Node(From.Node), FromLo, WindowHi(Tau, 0.0, Margin), ETaxiWay::Any);

		if (!To.bPushMove)
		{
			Out.MoveStarts.Add(Steps.Num());
		}
		for (int32 Index = 0; Index < To.Chain.Num(); ++Index)
		{
			const FChainStep& Step = To.Chain[Index];
			const bool bEnd = Index == To.Chain.Num() - 1;

			// The edge from entering it until the TAIL leaves it - for the chain's last edge, when the aircraft leaves
			// the node it may wait at; at the goal, never.
			const double EdgeHi = !bEnd ? WindowHi(Tau, Step.Reach, Margin)
				: bLast ? FTaxiReservations::Forever
				: FMath::Max(WindowHi(Tau, Step.Reach, Margin), WindowHi(LeavesEnd, 0.0, Margin));
			AddPass(FTaxiResource::Edge(Step.Edge), WindowLo(Tau, 0.0, Margin), EdgeHi, Step.Way);
			if (!bEnd)
			{
				AddPass(FTaxiResource::Node(Step.To), WindowLo(Tau, 0.0, Margin),
					WindowHi(Tau, Step.Reach, Margin), ETaxiWay::Any);
			}

			// THE PUSH IS NOT ON THE ROUTE: Route is what the aircraft TAXIES, from where the push ends - the push's own
			// route is PushbackPlanner's, which the caller already holds.
			if (To.bPushMove)
			{
				continue;
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

		if (To.bPushMove)
		{
			PushPasses = Out.Passes.Num();
		}

		if (bLast)
		{
			// The goal, for ever: it stays there.
			AddPass(FTaxiResource::Node(To.Node),
				WindowLo(Tau, MoveHoldsEndFromStart(To.bPushMove, To.Chain.Num()) ? 0.0 : To.Chain.Last().Reach, Margin), FTaxiReservations::Forever,
				ETaxiWay::Any);
		}
	}

	for (int32 Index = 0; Index < PushPasses; ++Index)
	{
		if (!PushJoined.Contains(Index))
		{
			Out.PushWindows.Add(Out.Passes[Index].Resource);
		}
	}
	Out.Route = RouteSearch::PlanFromSteps(Network, TaxiStart, MoveTemp(Steps));
	Out.Arrival = States[Reached].Arrive;
	Out.RouteResult = ERouteResult::Found;
	Out.Result = ETaxiPlanResult::Planned;
	return Out;
}
