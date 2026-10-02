#include "Model/TaxiPlanning.h"

#include "AirsideLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/GroundTraffic.h"
#include "Model/PassingOrder.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficRules.h"

namespace
{
	/**
	 * Whether the route an agent is following is still its plan's. Steps, ends and length - the shapes a replan, a
	 * splice or a redirect change - rather than every step: compared per agent per tick, and a route that kept its
	 * count, both ends and its length to a centimetre while changing a middle edge is not a change anything here makes.
	 */
	bool SameRoute(const FRoutePlan& Live, const FRoutePlan& Planned)
	{
		return Live.Steps.Num() == Planned.Steps.Num() && Live.Steps.Num() > 0
			&& Live.Steps[0].Edge == Planned.Steps[0].Edge && Live.Steps.Last().Edge == Planned.Steps.Last().Edge
			&& FMath::IsNearlyEqual(Live.Length, Planned.Length, 1.0);
	}

	/** The plan's route in taxiway names, consecutive repeats collapsed - "A,A3,B" (spec §3's clearance wording). */
	FString ViaText(const URoadNetwork& Network, const FRoutePlan& Route)
	{
		TArray<FString> Names;
		for (const FRouteStep& Step : Route.Steps)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			if (Edge == nullptr || Network.GetSegment(Edge->DerivedFrom) == nullptr || Network.IsRunwaySegment(Edge->DerivedFrom))
			{
				continue;
			}
			const FString Name = Network.TaxiwayDisplayName(Network.TaxiwayOf(Edge->DerivedFrom));
			if (!Name.IsEmpty() && (Names.Num() == 0 || Names.Last() != Name))
			{
				Names.Add(Name);
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(",")) : FString(TEXT("unnamed lines"));
	}

	/** Where a plan ends, as the log says it: a stand by its number, anything else by its node. */
	FString GoalText(const URoadNetwork& Network, const FRoutePlan& Route, ETaxiClearanceKind Kind)
	{
		const FGuidelineNodeId Goal = Route.Steps.Num() > 0 ? Route.Steps.Last().To : FGuidelineNodeId();
		const int32 Stand = Network.FindEntityIndexByPoseNode(Goal);
		if (Stand != INDEX_NONE && Network.GetEntities().IsValidIndex(Stand))
		{
			return FString::Printf(TEXT("stand %d"), Network.GetEntities()[Stand].StandNumber);
		}
		// A departure's goal is its runway entry - or, queueing, a holding node short of it (SetQueued says which).
		return Kind == ETaxiClearanceKind::TaxiOut ? FString::Printf(TEXT("out to node %d"), Goal.Index)
			: FString::Printf(TEXT("node %d"), Goal.Index);
	}
}

void UTaxiPlanning::SetHeadway(double Seconds)
{
	Table.SetHeadway(Seconds);
}

FTaxiPlan UTaxiPlanning::Plan(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
	const FTaxiRequest& Request) const
{
	FTaxiPlanner Planner(Network, Table, Airframe, Rules);
	return Planner.Plan(Request);
}

FTaxiPlan UTaxiPlanning::PlanArrival(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
	const FTaxiRequest& Request, TArray<int32>& OutRevoke) const
{
	OutRevoke.Reset();
	FTaxiPlan Planned = Plan(Network, Airframe, Rules, Request);
	if (Planned.IsPlanned() || Planned.Result == ETaxiPlanResult::NoRoute)
	{
		return Planned;
	}

	// ARRIVALS FIRST (ruling 2): plan again with every departure that has not started its push taken off the table -
	// a COPY, nothing given up yet - and revoke only those whose windows the plan then needs. A Moving plan is never
	// offered: everyone booked behind it waits on it, and an aircraft under way has no second plan to fall back on.
	TArray<int32> Unstarted;
	for (const TPair<int32, FTaxiClearance>& Each : Clearances)
	{
		if (Each.Value.Stage == ETaxiClearanceStage::Booked)
		{
			Unstarted.Add(Each.Key);
		}
	}
	if (Unstarted.Num() == 0)
	{
		return Planned;
	}
	FTaxiReservations Without = Table;
	for (const int32 Holder : Unstarted)
	{
		Without.ReleaseHolder(Holder);
	}
	FTaxiPlanner Planner(Network, Without, Airframe, Rules);
	FTaxiPlan Freed = Planner.Plan(Request);
	if (!Freed.IsPlanned())
	{
		return Planned;
	}
	for (const int32 Holder : Unstarted)
	{
		const bool bInTheWay = Freed.Passes.ContainsByPredicate([this, Holder](const FTaxiPass& Pass)
		{
			for (const FTaxiWindow& Window : Table.WindowsOn(Pass.Resource))
			{
				if (Window.Holder == Holder && !FTaxiReservations::MayShare(Window, Pass.Window, Table.GetHeadway()))
				{
					return true;
				}
			}
			return false;
		});
		if (bInTheWay)
		{
			OutRevoke.Add(Holder);
		}
	}
	return Freed;
}

bool UTaxiPlanning::Book(const URoadNetwork& Network, int32 Holder, ETaxiClearanceKind Kind, const FTaxiPlan& InPlan,
	ETaxiClearanceStage Stage, const FRoutePlan& PushRoute, double Now)
{
	FTaxiPlan Held = InPlan;
	for (FTaxiPass& Pass : Held.Passes)
	{
		Pass.Window.Holder = Holder;
	}
	// ON A COPY, THEN SWAPPED IN: the holder's old plan goes and the new one lands together, or neither does.
	FTaxiReservations Trial = Table;
	const bool bReplaced = Trial.ReleaseHolder(Holder) > 0;
	if (!Trial.BookPasses(Held.Passes))
	{
		UE_LOG(LogAirsideTaxiPlan, Warning, TEXT("TaxiPlan: agent %d's plan could not be booked - the table moved under it"), Holder);
		return false;
	}
	Table = MoveTemp(Trial);

	FTaxiClearance& Clearance = Clearances.Add(Holder);
	Clearance.Holder = Holder;
	Clearance.Kind = Kind;
	Clearance.Stage = Stage;
	Clearance.Plan = MoveTemp(Held);
	Clearance.PushRoute = PushRoute;

	// WHAT ONLY THE PUSH HOLDS - the planner's PushWindows - released, one window each, when the push hands over (Track).
	Clearance.PushOnly = Clearance.Plan.PushWindows;
	LastRefusal.Remove(Holder);
	// PLANNED AGAIN: no longer unplanned (spec §3 - the alert clears on a new plan).
	ClearUnplanned(Holder);
	// A RE-BOOKING MAY HAVE FREED what the old plan held (review of #528 finding 7): a waiting arrival hears of it.
	Bump(bReplaced);

	UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d planned %s via %s (eta %.0f s%s)"), Holder,
		*GoalText(Network, Clearance.Plan.Route, Kind), *ViaText(Network, Clearance.Plan.Route), Clearance.Plan.Arrival - Now,
		Stage == ETaxiClearanceStage::Booked ? *FString::Printf(TEXT(", pushes in %.0f s"), Clearance.Plan.PushAt - Now) : TEXT(""));
	return true;
}

bool UTaxiPlanning::BookArrival(const URoadNetwork& Network, int32 Holder, const FTaxiPlan& InPlan, TConstArrayView<int32> Revoke,
	double Now, TArray<int32>& OutRevoked)
{
	OutRevoked.Reset();
	// ONE SWAP: the revoked departures' windows go only if the arrival's plan then books - on a copy, as Book's own.
	const FTaxiReservations Before = Table;
	for (const int32 Departure : Revoke)
	{
		const FTaxiClearance* Clearance = Clearances.Find(Departure);
		if (Clearance != nullptr && Clearance->Stage == ETaxiClearanceStage::Booked)
		{
			Table.ReleaseHolder(Departure);
			OutRevoked.Add(Departure);
		}
	}
	if (!Book(Network, Holder, ETaxiClearanceKind::TaxiIn, InPlan, ETaxiClearanceStage::Moving, FRoutePlan(), Now))
	{
		Table = Before;
		OutRevoked.Reset();
		return false;
	}
	// FROM ITS RUNWAY EXIT, ON FINAL: its first move is not the order's to refuse (OrderHold), and no re-time moves it.
	FTaxiClearance& Cleared = Clearances[Holder];
	Cleared.bFromExit = true;
	Cleared.bOnFinal = true;
	for (const int32 Departure : OutRevoked)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d revoked by arrival %d"), Departure, Holder);
		Clearances.Remove(Departure);
		LastRefusal.Remove(Departure);
	}
	if (OutRevoked.Num() > 0)
	{
		Bump(true);
	}
	return true;
}

void UTaxiPlanning::MarkMoving(int32 Holder)
{
	if (FTaxiClearance* Clearance = Clearances.Find(Holder); Clearance != nullptr && Clearance->Stage == ETaxiClearanceStage::Booked)
	{
		Clearance->Stage = ETaxiClearanceStage::Moving;
	}
}

void UTaxiPlanning::SetQueued(int32 Holder, FGuidelineNodeId QueueFor, ERouteErrand Errand, const FVector2D& QueueAt)
{
	if (FTaxiClearance* Clearance = Clearances.Find(Holder))
	{
		Clearance->QueueFor = QueueFor;
		Clearance->QueueErrand = Errand;
		Clearance->QueueAt = QueueAt;
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d queues short of runway entry (node %d) - the entry is booked ahead of it"),
			Holder, QueueFor.Index);
	}
}

TArray<int32> UTaxiPlanning::QueuedDueAsk(double Now) const
{
	TArray<int32> Due;
	for (const TPair<int32, FTaxiClearance>& Each : Clearances)
	{
		const FTaxiClearance& Clearance = Each.Value;
		// EVENT-DRIVEN, COALESCED: asked when the table has moved since the last ask - something released or re-booked -
		// and not more than once a sim second, the push watch's bound for the same reason (FPushWatch::TaxiRevision).
		// DATED ONLY WHEN ASKED (NoteExtendAsked): one still pushing is listed and skipped, and must be listed again once
		// it taxis rather than wait for the table to move (review of #528 finding 7).
		if (Clearance.QueueFor.IsSet() && Clearance.Stage != ETaxiClearanceStage::Booked
			&& (Clearance.ExtendAskedAt < 0.0 || (Clearance.ExtendRevision != RevisionCount && Now - Clearance.ExtendAskedAt >= 1.0)))
		{
			Due.Add(Each.Key);
		}
	}
	return Due;
}

void UTaxiPlanning::NoteExtendAsked(int32 Holder, double Now)
{
	if (FTaxiClearance* Clearance = Clearances.Find(Holder))
	{
		Clearance->ExtendAskedAt = Now;
		Clearance->ExtendRevision = RevisionCount;
	}
}

bool UTaxiPlanning::Extend(const URoadNetwork& Network, int32 Holder, const FTaxiPlan& Ext, double Now)
{
	FTaxiClearance* Clearance = Clearances.Find(Holder);
	if (Clearance == nullptr || !Clearance->QueueFor.IsSet() || Clearance->Plan.Route.Steps.Num() == 0)
	{
		return false;
	}
	// WHAT IT HOLDS NOW, the queue's for-ever windows ended as the rest sets off (a gap's margin after), then the rest's
	// windows joined on - one window per resource, as the planner books (TaxiPlanner.cpp's AddPass).
	const double Leaves = Ext.PushAt + FMath::Max(0.0, Table.GetHeadway());
	TArray<FTaxiPass> Passes;
	auto Join = [&Passes, Holder](const FTaxiResource& Resource, FTaxiWindow Window)
	{
		Window.Holder = Holder;
		for (FTaxiPass& Pass : Passes)
		{
			if (Pass.Resource == Resource && Pass.Window.From <= Window.To && Window.From <= Pass.Window.To)
			{
				Pass.Window.From = FMath::Min(Pass.Window.From, Window.From);
				Pass.Window.To = FMath::Max(Pass.Window.To, Window.To);
				Pass.Window.Way = Pass.Window.Way == Window.Way ? Window.Way : ETaxiWay::Any;
				return;
			}
		}
		Passes.Add({ Resource, Window });
	};
	for (const FTaxiPass& Booked : Clearance->Plan.Passes)
	{
		for (const FTaxiWindow& Window : Table.WindowsOn(Booked.Resource))
		{
			if (Window.Holder == Holder)
			{
				FTaxiWindow Kept = Window;
				if (Kept.To >= FTaxiReservations::Forever)
				{
					Kept.To = FMath::Max(Kept.From + UE_KINDA_SMALL_NUMBER, Leaves);
				}
				Join(Booked.Resource, Kept);
			}
		}
	}
	for (const FTaxiPass& Pass : Ext.Passes)
	{
		Join(Pass.Resource, Pass.Window);
	}
	FTaxiReservations Trial = Table;
	Trial.ReleaseHolder(Holder);
	if (!Trial.BookPasses(Passes))
	{
		UE_LOG(LogAirsideTaxiPlan, Warning, TEXT("TaxiPlan: agent %d's way on to the runway could not be booked - the table moved under it"), Holder);
		return false;
	}
	Table = MoveTemp(Trial);

	// THE PLAN GROWS BY THE REST: its moves and legs after its own, its passes the whole booked set.
	const int32 Offset = Clearance->Plan.Route.Steps.Num();
	for (const int32 Start : Ext.MoveStarts)
	{
		Clearance->Plan.MoveStarts.Add(Offset + Start);
	}
	Clearance->Plan.Legs.Append(Ext.Legs);
	Clearance->Plan.Holds.Append(Ext.Holds);
	Clearance->Plan.Passes = MoveTemp(Passes);
	Clearance->Plan.Arrival = Ext.Arrival;
	Clearance->QueueFor = FGuidelineNodeId();
	// ITS FOR-EVER WINDOWS ENDED: time freed on its holding node and the edge into it (review of #528 finding 7).
	Bump(true);
	UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d planned on to runway entry (node %d) via %s (eta %.0f s)"), Holder,
		Ext.Route.Steps.Num() > 0 ? Ext.Route.Steps.Last().To.Index : INDEX_NONE, *ViaText(Network, Ext.Route), Ext.Arrival - Now);
	return true;
}

void UTaxiPlanning::AdoptRoute(int32 Holder, const FRoutePlan& Live)
{
	if (FTaxiClearance* Clearance = Clearances.Find(Holder))
	{
		Clearance->Plan.Route = Live;
	}
}

bool UTaxiPlanning::Revoke(int32 Holder, int32 ByArrival)
{
	const FTaxiClearance* Clearance = Clearances.Find(Holder);
	if (Clearance == nullptr || Clearance->Stage != ETaxiClearanceStage::Booked)
	{
		return false;
	}
	UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d revoked by arrival %d"), Holder, ByArrival);
	Table.ReleaseHolder(Holder);
	Clearances.Remove(Holder);
	Bump(true);
	return true;
}

void UTaxiPlanning::Drop(int32 Holder, const TCHAR* Why)
{
	DelayedUntilForTest.Remove(Holder);
	LastRefusal.Remove(Holder);
	const bool bHad = Clearances.Remove(Holder) > 0;
	const int32 Released = Table.ReleaseHolder(Holder);
	if (bHad && Why != nullptr)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d unplanned - %s"), Holder, Why);
	}
	if (bHad || Released > 0)
	{
		Bump(true);
	}
}

void UTaxiPlanning::DropAll(const TCHAR* Why)
{
	const int32 Count = Clearances.Num();
	TArray<int32> Holders;
	Clearances.GetKeys(Holders);
	for (const int32 Holder : Holders)
	{
		Drop(Holder, nullptr);
	}
	Table = FTaxiReservations();
	if (Count > 0)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: %d plan(s) unplanned - %s"), Count, Why);
	}
}

void UTaxiPlanning::NoteRefused(int32 Holder, const FString& What, const FString& Why)
{
	// ONCE PER REASON: a refusal is asked again on every event that might clear it, and the log wants the change.
	const FString Said = What + TEXT(" - ") + Why;
	FString& Last = LastRefusal.FindOrAdd(Holder);
	if (Last != Said)
	{
		Last = Said;
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: %s refused - %s"), *What, *Why);
	}
}

int32 UTaxiPlanning::WaitingFor(int32 Holder, const FTaxiResource& Resource) const
{
	if (!bEnforceOrderForTest)
	{
		return 0;
	}
	return FPassingOrder::WaitingFor(Table, Holder, Resource, [this, &Resource](int32 Other)
	{
		const FTaxiClearance* Clearance = Clearances.Find(Other);
		return Clearance != nullptr && Clearance->Entered.Contains(Resource);
	});
}

int32 UTaxiPlanning::OpposingAhead(int32 Holder, const FTaxiResource& Edge) const
{
	if (!bEnforceOrderForTest)
	{
		return 0;
	}
	const TConstArrayView<FTaxiWindow> Windows = Table.WindowsOn(Edge);
	const int32 Mine = Windows.IndexOfByPredicate([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; });
	for (int32 Index = 0; Index < Mine; ++Index)
	{
		// STILL IN THE TABLE IS NOT YET LEFT (released as its tail clears, Track). Any - a push, a merged window - may come
		// either way, so it counts as coming at it.
		const FTaxiWindow& Ahead = Windows[Index];
		if (Ahead.Holder != Holder && (Ahead.Way == ETaxiWay::Any || Windows[Mine].Way == ETaxiWay::Any || Ahead.Way != Windows[Mine].Way))
		{
			return Ahead.Holder;
		}
	}
	return 0;
}

int32 UTaxiPlanning::PushWaitingFor(int32 Holder) const
{
	const FTaxiClearance* Clearance = Clearances.Find(Holder);
	if (Clearance == nullptr)
	{
		return 0;
	}
	// THE PUSH'S MOVE, all of it: every window the plan holds from the push's start - the stand, the push ground and
	// the node the push ends on - must be its turn before the aircraft leaves the stand, the order's all-or-nothing.
	for (const FTaxiPass& Pass : Clearance->Plan.Passes)
	{
		if (Pass.Window.From <= Clearance->Plan.PushAt)
		{
			if (const int32 Blocker = WaitingFor(Holder, Pass.Resource))
			{
				return Blocker;
			}
		}
	}
	return 0;
}

TArray<int32> UTaxiPlanning::DuePushes(double Now) const
{
	TArray<int32> Due;
	for (const TPair<int32, FTaxiClearance>& Each : Clearances)
	{
		if (Each.Value.Stage == ETaxiClearanceStage::Booked && Each.Value.Plan.PushAt <= Now)
		{
			Due.Add(Each.Key);
		}
	}
	return Due;
}

TArray<int32> UTaxiPlanning::Holders() const
{
	TArray<int32> Out;
	Clearances.GetKeys(Out);
	return Out;
}

bool UTaxiPlanning::OrderHold(const FRoadAgent& Agent, double Now, double T, double Head, FTaxiOrderHold& Out)
{
	const FRoutePlan& Route = Agent.PlanInProgress();
	if (Route.Steps.Num() == 0)
	{
		return false;
	}
	const int32 Current = UGroundTraffic::CurrentStep(Route, T);

	// A TEST'S DELAY: held still where it stands - StepStart T makes the claim pass's stop "G short of here", floored at 0.
	if (const double* Until = DelayedUntilForTest.Find(Agent.Id))
	{
		if (Now < *Until)
		{
			Out.Step = Current;
			Out.StepStart = T;
			Out.Blocker = DelayBlocker;
			Out.Resource = FTrafficResource::OfEdge(Route.Steps[Current].Edge);
			return true;
		}
		DelayedUntilForTest.Remove(Agent.Id);
	}

	FTaxiClearance* Clearance = Clearances.Find(Agent.Id);
	if (Clearance == nullptr || Clearance->Stage != ETaxiClearanceStage::Moving || Agent.Phase != EAgentPhase::Taxiing
		|| !bEnforceOrderForTest || !SameRoute(Route, Clearance->Plan.Route))
	{
		return false;
	}

	// PER MOVE, ALL OR NOTHING, BEFORE IT IS ENTERED: every edge and every node it PASSES from one stoppable node to the
	// next must be its turn - and through a junction (a move of more than one edge) its end node too - so the order never
	// holds an aircraft inside a junction. A move it has been let into is granted for good (committed, below): it is in
	// it, and the order behind it waits on it. A LANE'S END NODE IS ASKED ON ITS OWN, as the window reaches it: the lane is
	// one the aircraft may stand on (FTaxiPlanner::CanHoldAt), so it waits a gap short of it, on its own lane. Every wait therefore points at
	// a move booked to start - or a node booked to be reached - EARLIER on the one timeline (TaxiPlanner.cpp's needs):
	// no cycle can form. ENFORCED BY: Airside.Model.TaxiPlan.NotMyTurn, Airside.Perf.TaxiPlan.NoPermanentDeadlock
	const TArray<int32>& Moves = Clearance->Plan.MoveStarts;
	for (int32 Move = 0; Move < Moves.Num(); ++Move)
	{
		const int32 First = Moves[Move];
		const int32 Last = (Moves.IsValidIndex(Move + 1) ? Moves[Move + 1] : Route.Steps.Num()) - 1;
		if (!Route.Steps.IsValidIndex(First) || !Route.Steps.IsValidIndex(Last) || Last < Current)
		{
			continue;
		}
		const double Start = UGroundTraffic::StepStart(Route, First);
		if (First > Current && Start >= Head)
		{
			break;
		}
		// AN ARRIVAL'S FIRST MOVE IS NEVER REFUSED BY THE ORDER (review of #528 finding 4): it vacates rolling and may not
		// wait (bMayWaitAtStart false) - a hold there is a hold ON THE RUNWAY, and whoever runs late ahead of it may need that
		// runway: order against runway. It goes; the claim pass's spacing still keeps it off any body there. ONLY A PLAN THAT
		// STARTS AT THE EXIT (bFromExit - review of #534 finding 3: move 0 of a re-plan is wherever the aircraft is). AND NOT
		// AGAINST ONE COMING THE OTHER WAY (finding 7): late and waiting at the far end, it would drive on in its turn and
		// meet the arrival head-on mid-edge, for good - held at the exit is the lesser harm.
		// ENFORCED BY: Airside.Model.TaxiPlan.ArrivalNeverHeldOnTheExit, Airside.Model.TaxiPlan.ReplannedArrivalIsOrderedMidRoute
		const bool bExitMove = Move == 0 && Clearance->bFromExit;
		if (bExitMove && Move > Clearance->GrantedMove)
		{
			for (int32 Index = First; Index <= Last; ++Index)
			{
				if (const int32 Opposer = OpposingAhead(Agent.Id, FTaxiResource::Edge(Route.Steps[Index].Edge)))
				{
					Out.Step = First;
					Out.StepStart = Start;
					Out.Blocker = Opposer;
					Out.Resource = FTrafficResource::OfEdge(Route.Steps[Index].Edge);
					return true;
				}
			}
		}
		if (Move > Clearance->GrantedMove)
		{
			for (int32 Index = First; Index <= Last && !bExitMove; ++Index)
			{
				const FRouteStep& Step = Route.Steps[Index];
				int32 Blocker = WaitingFor(Agent.Id, FTaxiResource::Edge(Step.Edge));
				FTrafficResource On = FTrafficResource::OfEdge(Step.Edge);
				// THE END NODE TOO, for a move of more than one edge: booked from the move's start (TaxiPlanner.cpp's needs), so
				// asked with the rest - a junction is never waited out half-way through.
				if (Blocker == 0 && (Index < Last || Last > First))
				{
					Blocker = WaitingFor(Agent.Id, FTaxiResource::Node(Step.To));
					On = FTrafficResource::OfNode(Step.To);
				}
				if (Blocker != 0)
				{
					Out.Step = First;
					Out.StepStart = Start;
					Out.Blocker = Blocker;
					Out.Resource = On;
					return true;
				}
			}
			// LET IN FOR GOOD AS SOON AS ITS WINDOW REACHES THE MOVE - within its stopping distance - not once its centre is
			// on it (review of #528 finding 2): between the two, a plan booked ahead of an EARLY aircraft's late window there
			// turned the order against it with no room left to stop, and it halted on the move's approach, its claims
			// blocking the newcomer that its order now waited on. Committed, its windows there start now: nothing booked from
			// here on can be placed ahead of it. ENFORCED BY: Airside.Model.TaxiPlan.CommittedMoveIsNotReordered
			Clearance->GrantedMove = Move;
			bool bPulled = false;
			for (int32 Index = First; Index <= Last; ++Index)
			{
				bPulled |= Table.PullForward(FTaxiResource::Edge(Route.Steps[Index].Edge), Agent.Id, Now);
				if (Index < Last || Last > First)
				{
					bPulled |= Table.PullForward(FTaxiResource::Node(Route.Steps[Index].To), Agent.Id, Now);
				}
			}
			if (bPulled)
			{
				Bump(false);
			}
		}
		const FRouteStep& EndStep = Route.Steps[Last];
		const bool bEndAskedWithBody = Last > First;
		// QUEUEING AT THE PLAN'S END: held a gap short of its holding node until the rest is booked - else it would drive
		// to the end of its route and park on a taxiway. It waits for whoever holds the entry it queues for.
		if (Clearance->QueueFor.IsSet() && Last == Route.Steps.Num() - 1)
		{
			const TConstArrayView<FTaxiWindow> OnEntry = Table.WindowsOn(FTaxiResource::Node(Clearance->QueueFor));
			Out.Step = Last;
			Out.StepStart = UGroundTraffic::StepStart(Route, Last);
			Out.StepEnd = EndStep.EndDistance;
			Out.bAtNode = true;
			Out.Blocker = OnEntry.Num() > 0 && OnEntry[0].Holder != Agent.Id ? OnEntry[0].Holder : QueueBlocker;
			Out.Resource = FTrafficResource::OfNode(EndStep.To);
			return true;
		}
		const bool bEndGranted = bEndAskedWithBody || Move <= Clearance->GrantedEnd || bExitMove;
		if (const int32 Blocker = bEndGranted ? 0 : WaitingFor(Agent.Id, FTaxiResource::Node(EndStep.To)))
		{
			Out.Step = Last;
			Out.StepStart = UGroundTraffic::StepStart(Route, Last);
			Out.StepEnd = EndStep.EndDistance;
			Out.bAtNode = true;
			Out.Blocker = Blocker;
			Out.Resource = FTrafficResource::OfNode(EndStep.To);
			return true;
		}
		// A LANE'S END NODE, its turn and inside the window: committed as a move is, above, for the same reason.
		if (!bEndGranted && EndStep.EndDistance < Head)
		{
			Clearance->GrantedEnd = Move;
			if (Table.PullForward(FTaxiResource::Node(EndStep.To), Agent.Id, Now))
			{
				Bump(false);
			}
		}
		if (EndStep.EndDistance >= Head)
		{
			break;
		}
	}
	return false;
}

bool UTaxiPlanning::Track(const FRoadAgent& Agent, const FTrafficOccupancy& Occupancy, const FTrafficRules& Rules, double Now)
{
	FTaxiClearance* Clearance = Clearances.Find(Agent.Id);
	if (Clearance == nullptr)
	{
		return true;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	switch (Agent.Phase)
	{
	case EAgentPhase::Arriving:
		// ON FINAL: its landing is flown, so no re-time may move its windows (FTaxiReservations::ShiftLater's Immovable).
		Clearance->bOnFinal = true;
		return true;
	case EAgentPhase::Manoeuvring:
		Clearance->Stage = ETaxiClearanceStage::Moving;
		return true;
	case EAgentPhase::Parked:
		// AT ITS GOAL: its stand (and the lead-in its tail is on) stay booked for ever; nothing more is ordered. A Booked
		// departure parked on its stand is waiting for its push, and stays Booked.
		if (Clearance->Stage == ETaxiClearanceStage::Moving)
		{
			Clearance->Stage = ETaxiClearanceStage::Done;
		}
		return true;
	case EAgentPhase::Departing:
		// LINED UP: the runway is the runway's own authority from here (the bar chain, the occupancy), and the entry its
		// plan held for ever is the next departure's to plan to.
		Drop(Agent.Id, nullptr);
		return true;
	case EAgentPhase::Taxiing:
		break;
	case EAgentPhase::Reversing:
	case EAgentPhase::Gone:
	case EAgentPhase::Stranded:
		// NOT ON ITS PLAN ANY MORE: an aircraft never reverses on a taxiway, and a stranded or gone one taxis nowhere.
		Drop(Agent.Id, TEXT("it is no longer taxiing on its plan (stranded)"));
		return true;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	const FRoutePlan& Route = Agent.PlanInProgress();
	if (Clearance->Stage == ETaxiClearanceStage::Done || !SameRoute(Route, Clearance->Plan.Route))
	{
		return false;
	}
	Clearance->Stage = ETaxiClearanceStage::Moving;
	Clearance->bOnFinal = false;

	bool bReleased = false;
	for (const FTaxiResource& Resource : Clearance->PushOnly)
	{
		// ITS EARLIEST WINDOW THERE IS THE PUSH'S: the planner books the push from PushAt, before every taxi window. A later
		// one is the taxi's, still due. ENFORCED BY: Airside.Model.TaxiPlan.PushPrefixHoldsAtStand
		bReleased |= Table.ReleaseFirstOn(Resource, Agent.Id);
	}
	Clearance->PushOnly.Reset();

	const double F = Rules.FootprintFor(Agent.Class);
	const double T = FClaimPass::CentreOf(Agent);
	const double Nose = T + F * 0.5;
	const double Tail = T - F * 0.5;

	// ENTERED: its nose is past the edge's start - what a follower booked the same way behind it waits for.
	for (int32 Index = FMath::Max(0, Clearance->ReleasedThrough + 1); Index < Route.Steps.Num(); ++Index)
	{
		if (UGroundTraffic::StepStart(Route, Index) > Nose)
		{
			break;
		}
		Clearance->Entered.Add(FTaxiResource::Edge(Route.Steps[Index].Edge));
	}

	// RELEASED WHEN THE TAIL CLEARS IT (spec §1): behind the tail AND no longer claimed - the claim pass keeps a node a
	// body's reach past it, and that is what "cleared" means here too. Never the last step: the goal and the edge onto it
	// are held for ever (a departure's go when it lines up, above).
	auto Held = [&Occupancy, &Agent](const FTrafficResource& Resource)
	{
		return Occupancy.FindClaim(Agent.Id, Resource) != nullptr;
	};
	if (!Clearance->bStartReleased && Tail > 0.0)
	{
		const FGuidelineNodeId Start = UGroundTraffic::StepFromNode(Route, 0);
		if (!Held(FTrafficResource::OfNode(Start)))
		{
			bReleased |= Table.ReleaseFirstOn(FTaxiResource::Node(Start), Agent.Id);
			Clearance->bStartReleased = true;
		}
	}
	for (int32 Index = Clearance->ReleasedThrough + 1; Index < Route.Steps.Num() - 1; ++Index)
	{
		const FRouteStep& Step = Route.Steps[Index];
		if (Tail <= Step.EndDistance || Held(FTrafficResource::OfEdge(Step.Edge)) || Held(FTrafficResource::OfNode(Step.To)))
		{
			break;
		}
		// ONCE PER PASS (ReleaseFirstOn): a turnaround loop passes one edge out and back, and the way back is still due.
		bReleased |= Table.ReleaseFirstOn(FTaxiResource::Edge(Step.Edge), Agent.Id);
		bReleased |= Table.ReleaseFirstOn(FTaxiResource::Node(Step.To), Agent.Id);
		Clearance->Entered.Remove(FTaxiResource::Edge(Step.Edge));
		Clearance->ReleasedThrough = Index;
	}
	if (bReleased)
	{
		Bump(true);
	}

	// LATE BY MORE THAN THE KNOB: RE-TIMED (spec §2). Late = past when its plan has it leave the end of the step it is on
	// (the next leg's Leave - a planned hold there is not lateness), by the time it still needs to get there at taxi speed.
	// Not while it queues at its plan's end for a runway entry: those windows are for ever, and waiting is the plan.
	const int32 Current = UGroundTraffic::CurrentStep(Route, T);
	const bool bQueueing = Clearance->QueueFor.IsSet() && Current == Route.Steps.Num() - 1;
	if (!bQueueing && Clearance->Plan.Legs.Num() == Route.Steps.Num() && Route.Steps.IsValidIndex(Current))
	{
		const double Latest = Clearance->Plan.Legs.IsValidIndex(Current + 1) ? Clearance->Plan.Legs[Current + 1].Leave
			: Clearance->Plan.Legs[Current].Reach;
		if (Now > Latest + Rules.TaxiPlanRetimeLag
			&& (Clearance->RetimeTriedAt < 0.0 || Now - Clearance->RetimeTriedAt >= Clearance->RetimeBackoff))
		{
			Clearance->RetimeTriedAt = Now;
			const FAirframe* Aircraft = Agent.AsAircraft();
			const double Speed = Aircraft != nullptr ? FMath::Max(Aircraft->Chassis.Ground.Taxi.SpeedCap, 1.0) : 1000.0;
			const double Rest = FMath::Max(0.0, Route.Steps[Current].EndDistance - T);
			// FROM THE MOMENT IT IS LATE FOR (Since just before Latest), not from now: the overdue leave and the windows
			// that straddle it move with the rest, so the plan reads on time again after it (measured: from now, the overdue
			// leave never moved and it was re-timed every second by a growing lag - 709 re-times at 40/h, the field jammed).
			const bool bRetimed = Retime(Agent.Id, Latest - 0.001, Now + Rest / Speed - Latest, Now);
			// BACKED OFF WHILE REFUSED (review of #534 finding 9): Retime changes no clearance's place in the map.
			Clearance->RetimeBackoff = bRetimed ? 1.0 : FMath::Min(2.0 * Clearance->RetimeBackoff, 30.0);
		}
	}
	return true;
}

bool UTaxiPlanning::TakeReleased()
{
	const bool bWas = bReleasedSinceAsked;
	bReleasedSinceAsked = false;
	return bWas;
}

bool UTaxiPlanning::BookPassesForTest(TConstArrayView<FTaxiPass> Passes)
{
	const bool bBooked = Table.BookPasses(Passes);
	Bump(false);
	return bBooked;
}

void UTaxiPlanning::Bump(bool bReleasedSomething)
{
	++RevisionCount;
	bReleasedSinceAsked |= bReleasedSomething;
}
