// UGroundTraffic's half of a taxi plan the world changed (taxi planning PR 3, spec 2026-10-02 §2): the re-plan along the
// route an aircraft is already driving - from a route change, a layout edit or an unplanned retry - with whoever it would
// have trapped behind it re-planned after it, and the rebuild's re-plan in the old order. Split from
// GroundTrafficPlanning.cpp (the clearances: arrival, departure, queue, push) for rule 77's 800-line budget when the
// review of #534 grew ReplanTaxi - one class's two concerns, #427's pattern.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TaxiPlanning.h"
#include "Model/TrafficClaims.h"

namespace
{
	/**
	 * How far Agent's centre is past the start of Edge along its own route, uu - beyond the edge's length once it is past
	 * it - and which way it drives it: the pass of Edge at or behind where it is now. False when its route has not reached
	 * Edge by there (it is still coming, or never will).
	 */
	bool ProgressOnEdge(const FRoadAgent& Agent, FGuidelineEdgeId Edge, double& OutProgress, bool& bOutReversed)
	{
		const FRoutePlan& Route = Agent.PlanInProgress();
		const double T = FClaimPass::CentreOf(Agent);
		for (int32 Index = FMath::Min(UGroundTraffic::CurrentStep(Route, T), Route.Steps.Num() - 1); Index >= 0; --Index)
		{
			if (Route.Steps[Index].Edge == Edge)
			{
				OutProgress = T - UGroundTraffic::StepStart(Route, Index);
				bOutReversed = Route.Steps[Index].bReversed;
				return true;
			}
		}
		return false;
	}
}

bool UGroundTraffic::ReplanTaxi(const FRoadAgent& Agent, const URoadNetwork& Network, ETaxiUnplanned Cause, const TCHAR* Why,
	const FTaxiClearance* Old, int32 Depth)
{
	const FAirframe* Aircraft = Agent.AsAircraft();
	if (TaxiPlanning == nullptr || Aircraft == nullptr)
	{
		return false;
	}
	// WHAT IT WAS CLEARED FOR: from its old clearance, else from the unplanned record a retry keeps.
	const FTaxiUnplanned* Record = TaxiPlanning->FindUnplanned(Agent.Id);
	const ETaxiClearanceKind Kind = Old != nullptr ? Old->Kind : Record != nullptr ? Record->Kind
		: Agent.bDepartureArmed ? ETaxiClearanceKind::TaxiOut : ETaxiClearanceKind::TaxiIn;
	ERouteErrand Errand = Old != nullptr ? Old->QueueErrand : Record != nullptr ? Record->Errand : ERouteErrand::Unset;
	if (Errand == ERouteErrand::Unset)
	{
		// THE TAXI-IN ERRAND FROM ITS ONE OWNER (ArrivalPlanner::TaxiInRequest - rule 4), not spelled here.
		Errand = Kind == ETaxiClearanceKind::TaxiIn ? ArrivalPlanner::TaxiInRequest(FArrivalPlan(), 0.0).Errand
			: ERouteErrand::DepartureToEntry;
	}
	const FString Because = Why;
	auto Fail = [this, &Agent, Cause, &Because, Kind, Errand]()
	{
		TaxiPlanning->MarkUnplanned(Agent.Id, Cause, Because, Kind, Errand);
		return false;
	};
	const double Margin = FMath::Max(0.0, Rules.TaxiPlanMargin);
	// TO A RUNWAY ENTRY, HELD THERE A WHILE, NOT FOR EVER - as its first booking was (review of #534 finding 5). A route that
	// ends on a runway is what arms the take-off (ArmDepartureIfRunway); a queue's end, short of it, is held for ever.
	const double EntryHold = Kind == ETaxiClearanceKind::TaxiOut && Agent.bDepartureArmed ? Rules.TaxiPlanEntryHold : 0.0;

	// PARKED: its stand held for ever, as a plan that arrived there holds it; a departure booked to push later is back
	// on the push watch, re-planned when it is asked again (as a revoked one is).
	if (Agent.Phase == EAgentPhase::Parked)
	{
		if (Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
		{
			TaxiPlanning->Drop(Agent.Id, nullptr);
			return false;
		}
		FTaxiPlan Stay;
		Stay.Result = ETaxiPlanResult::Planned;
		Stay.Passes = { { FTaxiResource::Node(Agent.GoalNode), { Agent.Id, SimSeconds, FTaxiReservations::Forever } } };
		const bool bHeld = TaxiPlanning->Book(Network, Agent.Id, Kind, Stay, ETaxiClearanceStage::Done, FRoutePlan(), SimSeconds);
		if (Old != nullptr && Old->Stage == ETaxiClearanceStage::Booked)
		{
			FPushWatch& Watch = PushWatch.Add(Agent.Id);
			Watch.Network = &Network;
		}
		return bHeld;
	}

	FTaxiRequest Request;
	Request.Errand = Errand;
	Request.Holder = Agent.Id;
	FRoutePlan Live;
	TArray<FTaxiPass> PrefixPasses;
	FRoutePlan PushRoute;
	TArray<FTaxiResource> PushOnly;
	bool bFromExit = false;
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	switch (Agent.Phase)
	{
	case EAgentPhase::Taxiing:
		return ReplanTaxiing(Agent, Network, *Aircraft, Kind, Request, Old, EntryHold, Depth, Fail);
	case EAgentPhase::Arriving:
		// ON FINAL: from its exit, rolling, at the vacate time its first plan had - the landing is flown, not re-estimated,
		// and no re-time moves an arrival on final (FTaxiClearance::bOnFinal), so that time is still the flown one.
		if (Old == nullptr || !Agent.TaxiInPlan.IsValid())
		{
			return Fail();
		}
		Live = Agent.TaxiInPlan;
		Request.DepartAt = Old->Plan.PushAt;
		Request.bMayWaitAtStart = false;
		Request.bStartsRolling = true;
		bFromExit = true;
		break;
	case EAgentPhase::Manoeuvring:
	{
		// PUSHING: from where the push ends, when it ends; the push's ground held from now till then, released at the
		// hand-over as a planned push's is (PushOnly).
		if (!Agent.TaxiOutPlan.IsValid())
		{
			return Fail();
		}
		Live = Agent.TaxiOutPlan;
		PushRoute = Agent.Pushback.Plan;
		const double PushLeft = FMath::Max(0.0, PushRoute.Length - Agent.Pushback.Travelled)
			/ FMath::Max(Rules.PushSpeedFor(Aircraft->PushbackNeed), 1.0);
		Request.DepartAt = SimSeconds + PushLeft;
		for (const FRouteStep& Step : PushRoute.Steps)
		{
			for (const FTaxiResource& Resource : { FTaxiResource::Edge(Step.Edge), FTaxiResource::Node(Step.To) })
			{
				PrefixPasses.Add({ Resource, { Agent.Id, SimSeconds, Request.DepartAt + Margin, ETaxiWay::Any } });
				PushOnly.Add(Resource);
			}
		}
		break;
	}
	case EAgentPhase::Parked:      // above
	case EAgentPhase::Departing:   // lined up: the runway's own
	case EAgentPhase::Reversing:   // never an aircraft on a taxiway
	case EAgentPhase::Gone:
	case EAgentPhase::Stranded:
		TaxiPlanning->Drop(Agent.Id, nullptr);
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	Request.Start = StepFromNode(Live, 0);
	Request.Goal = Live.Steps.Num() > 0 ? Live.Steps.Last().To : FGuidelineNodeId();
	Request.Along = Live.Steps;
	const FTaxiPlan Tail = TaxiPlanning->bRefuseReplansForTest ? FTaxiPlan() : TaxiPlanning->Plan(Network, *Aircraft, Rules, Request);
	if (!Tail.IsPlanned() || !TaxiPlanning->BookAlong(Network, Agent.Id, Kind, ETaxiClearanceStage::Moving, Live, 0, Tail,
		PrefixPasses, PushRoute, PushOnly, SimSeconds, bFromExit, EntryHold))
	{
		return Fail();
	}
	return true;
}

bool UGroundTraffic::ReplanTaxiing(const FRoadAgent& Agent, const URoadNetwork& Network, const FAirframe& Aircraft,
	ETaxiClearanceKind Kind, FTaxiRequest Request, const FTaxiClearance* Old, double EntryHold, int32 Depth, TFunctionRef<bool()> Fail)
{
	// FROM THE END OF THE STEP IT IS ON, when it gets there at its speed (floored: a stopped aircraft still sets off); that
	// step is held from where it stood in the order until it leaves - the prefix.
	const FRoutePlan& Live = Agent.PlanInProgress();
	const double T = FClaimPass::CentreOf(Agent);
	const int32 Current = CurrentStep(Live, T);
	if (!Live.Steps.IsValidIndex(Current))
	{
		return Fail();
	}
	const FRouteStep& On = Live.Steps[Current];
	const FTaxiResource OnEdge = FTaxiResource::Edge(On.Edge);
	const FTaxiResource OnEnd = FTaxiResource::Node(On.To);
	const double Margin = FMath::Max(0.0, Rules.TaxiPlanMargin);
	const double Cap = FMath::Max(Aircraft.Chassis.Ground.Taxi.SpeedCap, 1.0);
	const double H = TaxiPlanning->GetTable().GetHeadway();
	const double Speed = FMath::Abs(Agent.SpeedAlongPlan());
	double Reach = SimSeconds + FMath::Max(0.0, On.EndDistance - T) / FMath::Max(Speed, 0.25 * Cap);
	const ETaxiWay Way = On.bReversed ? ETaxiWay::BToA : ETaxiWay::AToB;
	const int32 Prefix = Current + 1;

	// WHO ELSE IS BOOKED ON THE EDGE IT STANDS ON (review of #534 finding 1). A LEADER - taxiing that edge the same way and
	// physically ahead of it - stays ahead in the order. EVERYONE ELSE (behind it, still coming, or coming the other way)
	// must come after it there and at the node ahead: a re-plan made round them as they were booked placed it after a
	// follower that could not pass it - it waited for that one at the node, that one waited for its body: for good. So the
	// plan is made round the table WITHOUT them, and those it then conflicts with, or comes after on this edge or its end
	// node, are re-planned behind it.
	const double Mine = T - StepStart(Live, Current);
	double PrefixFrom = SimSeconds;
	double PrefixToFloor = 0.0;
	TArray<FTaxiWindow> Leaders;
	TArray<int32> Others;
	for (const FTaxiWindow& Window : TaxiPlanning->GetTable().WindowsOn(OnEdge))
	{
		if (Window.Holder == Agent.Id)
		{
			// WHERE IT ALREADY STOOD IN THE EDGE'S ORDER - it entered before anyone now behind it.
			PrefixFrom = FMath::Min(PrefixFrom, Window.From);
			continue;
		}
		const FRoadAgent* Other = FindAgent(Window.Holder);
		double Progress = 0.0;
		bool bReversed = false;
		const bool bLeader = Other != nullptr && Other->Phase == EAgentPhase::Taxiing && Window.Way == Way
			&& ProgressOnEdge(*Other, On.Edge, Progress, bReversed) && bReversed == On.bReversed && Progress > Mine;
		if (bLeader)
		{
			Leaders.Add(Window);
		}
		else
		{
			Others.AddUnique(Window.Holder);
		}
	}
	for (const FTaxiWindow& Leader : Leaders)
	{
		// FIFO BEHIND EACH LEADER, a headway apart at both ends (FTaxiReservations::MayShare).
		PrefixFrom = FMath::Max(PrefixFrom, Leader.From + H);
		PrefixToFloor = Leader.To < FTaxiReservations::Forever ? FMath::Max(PrefixToFloor, Leader.To + H) : FTaxiReservations::Forever;
	}
	const FTaxiReservations Around = TaxiPlanning->TableWithout(Others);

	FTaxiPlan Tail;
	TArray<FTaxiPass> PrefixPasses;
	if (Prefix == Live.Steps.Num())
	{
		// ON ITS LAST STEP: nothing left to plan but staying - its goal, and the step under it, for ever.
		Tail.Result = ETaxiPlanResult::Planned;
		Tail.Passes = { { OnEnd, { Agent.Id, Reach - Margin, FTaxiReservations::Forever } } };
		Tail.PushAt = Reach;
		Tail.Arrival = Reach;
		PrefixPasses.Add({ OnEdge, { Agent.Id, FMath::Min(PrefixFrom, SimSeconds), FTaxiReservations::Forever, Way } });
	}
	else
	{
		// SOMEBODY ON THE NODE AHEAD WHEN IT WOULD GET THERE: it waits for them where it is, on its own step.
		double Shift = 0.0;
		if (!Around.EarliestFit(OnEnd, ETaxiWay::Any, Reach, Reach + 0.001, Agent.Id, Shift))
		{
			return Fail();
		}
		Reach += Shift;
		Request.Start = On.To;
		Request.Goal = Live.Steps.Last().To;
		Request.DepartAt = Reach;
		Request.bStartsRolling = Shift <= 0.0 && Speed > 100.0;
		Request.bMayWaitAtStart = FTaxiPlanner::CanHoldAt(Network, Rules, On.Edge, On.To);
		Request.Along.Append(Live.Steps.GetData() + Prefix, Live.Steps.Num() - Prefix);
		Tail = TaxiPlanning->bRefuseReplansForTest ? FTaxiPlan() : UTaxiPlanning::PlanOn(Around, Network, Aircraft, Rules, Request);
		if (!Tail.IsPlanned() || PrefixToFloor >= FTaxiReservations::Forever)
		{
			return Fail();
		}
		// THE STEP IT IS ON, from where it stood in the order until it leaves - and a headway after any leader leaves it.
		const double To = FMath::Max(Tail.PushAt + Margin, PrefixToFloor);
		PrefixPasses.Add({ OnEdge, { Agent.Id, FMath::Min(PrefixFrom, To - 0.001), To, Way } });
	}

	// WHO MUST GO BEHIND IT: anyone it now conflicts with anywhere, or who would come before it on the edge it is on or at
	// that edge's end - ground they cannot reach without passing it.
	TArray<int32> Behind;
	for (const int32 Other : Others)
	{
		bool bMust = false;
		for (const TArray<FTaxiPass>* Set : { &PrefixPasses, &Tail.Passes })
		{
			for (const FTaxiPass& Ours : *Set)
			{
				const bool bOursFirst = Ours.Resource == OnEdge || Ours.Resource == OnEnd;
				for (const FTaxiWindow& Window : TaxiPlanning->GetTable().WindowsOn(Ours.Resource))
				{
					bMust |= Window.Holder == Other && (!FTaxiReservations::MayShare(Ours.Window, Window, H)
						|| (bOursFirst && Window.From < Ours.Window.From));
				}
			}
		}
		if (bMust)
		{
			Behind.Add(Other);
		}
	}

	// TAKEN OUT, IT BOOKED, THEY RE-PLANNED BEHIND IT - nearest it first, so each is placed behind the one ahead of it.
	TMap<int32, FTaxiClearance> Was;
	for (const int32 Other : Behind)
	{
		FTaxiClearance Taken;
		if (TaxiPlanning->TakeOut(Other, Taken))
		{
			Was.Add(Other, MoveTemp(Taken));
		}
	}
	// STILL ON ITS WAY OFF THE RUNWAY: an arrival re-planned on its exit move keeps that move's exemption (OrderHold).
	const bool bFromExit = Old != nullptr && Old->bFromExit
		&& Current < (Old->Plan.MoveStarts.IsValidIndex(1) ? Old->Plan.MoveStarts[1] : Live.Steps.Num());
	const bool bBooked = TaxiPlanning->BookAlong(Network, Agent.Id, Kind, ETaxiClearanceStage::Moving, Live, Prefix, Tail,
		PrefixPasses, FRoutePlan(), {}, SimSeconds, bFromExit, EntryHold);
	if (!bBooked)
	{
		Fail();
	}
	if (Behind.Num() > 0)
	{
		auto NearestFirst = [this, &On](int32 Id)
		{
			const FRoadAgent* Other = FindAgent(Id);
			double Progress = 0.0;
			bool bReversed = false;
			return Other != nullptr && ProgressOnEdge(*Other, On.Edge, Progress, bReversed) ? Progress : -TNumericLimits<double>::Max();
		};
		Behind.Sort([&NearestFirst](int32 L, int32 R) { return NearestFirst(L) > NearestFirst(R); });
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d's re-plan puts %d aircraft behind it on its edge - re-planned after it"),
			Agent.Id, Behind.Num());
		const FString Behindness = FString::Printf(TEXT("re-planned behind agent %d, ahead of it on its edge, and no plan fits"), Agent.Id);
		for (const int32 Other : Behind)
		{
			const FRoadAgent* Follower = FindAgent(Other);
			const FTaxiClearance* Had = Was.Find(Other);
			if (Follower == nullptr || Had == nullptr)
			{
				continue;
			}
			// A BOUND ON THE CHAIN: each re-plan may put its own followers behind it in turn; a few deep is a column of
			// aircraft on one edge, and past it the rest taxi unplanned (claims and resolver) rather than recurse further.
			if (Depth >= 3)
			{
				TaxiPlanning->MarkUnplanned(Other, ETaxiUnplanned::RouteChanged, Behindness, Had->Kind, Had->QueueErrand);
				continue;
			}
			if (ReplanTaxi(*Follower, Network, ETaxiUnplanned::RouteChanged, *Behindness, Had, Depth + 1))
			{
				RequeueAfterReplan(*Follower, Network, *Had);
			}
		}
	}
	return bBooked;
}

void UGroundTraffic::RequeueAfterReplan(const FRoadAgent& Agent, const URoadNetwork& Network, const FTaxiClearance& Old)
{
	// STILL QUEUEING, if its route still ends short of the entry: the entry re-found by where it was when the handle died
	// in a rebuild, as the re-resolve re-finds every node.
	const FTaxiClearance* Now = TaxiPlanning->Find(Agent.Id);
	if (Now == nullptr || !Old.QueueFor.IsSet() || Now->Plan.Route.Steps.Num() == 0)
	{
		return;
	}
	FGuidelineNodeId Entry = Old.QueueFor;
	if (Network.GetGuidelineNode(Entry) == nullptr)
	{
		Entry = RouteSearch::FindNearestNode(Network, Old.QueueAt, Agent.Class, Rules.ResolveRadius);
	}
	if (Entry.IsSet() && Now->Plan.Route.Steps.Last().To != Entry)
	{
		TaxiPlanning->SetQueued(Agent.Id, Entry, Old.QueueErrand, Old.QueueAt);
	}
}

void UGroundTraffic::ReplanAfterRebuild(const URoadNetwork& Network, const TArray<int32>& Order, const TMap<int32, FTaxiClearance>& Old)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	// THE PARKED FIRST, THEN THE MOVING, each pass IN THE ORDER THEY HELD - each planned round those already re-booked, as
	// each was booked behind them before. Nothing else runs inside OnGraphRebuilt, so nobody new is admitted ahead of them.
	// PARKED FIRST (review of #534 finding 4): a body on a stand is where it is. Re-made after the moving, an arrival
	// inbound to a stand a booked departure still stood on re-booked it for ever ahead of the departure - which could then
	// never plan its push before the arrival, while the arrival could never reach a stand with a body on it.
	// ENFORCED BY: Airside.Model.TaxiPlan.RebuildReholdsABookedStandFirst
	int32 Replanned = 0;
	int32 Lost = 0;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (const int32 Id : Order)
		{
			const FRoadAgent* Agent = FindAgent(Id);
			const FTaxiClearance* Was = Old.Find(Id);
			if (Agent == nullptr || Was == nullptr || (Agent->Phase == EAgentPhase::Parked) != (Pass == 0))
			{
				continue;
			}
			if (ReplanTaxi(*Agent, Network, ETaxiUnplanned::LayoutEdit, TEXT("a layout edit rebuilt the taxiways under it"), Was))
			{
				RequeueAfterReplan(*Agent, Network, *Was);
				++Replanned;
			}
			else
			{
				Lost += TaxiPlanning->FindUnplanned(Id) != nullptr ? 1 : 0;
			}
		}
	}
	if (Order.Num() > 0)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: layout rebuilt - %d plan(s) re-made in their old order, %d unplanned"),
			Replanned, Lost);
	}
}

void UGroundTraffic::RetryUnplanned(const URoadNetwork& Network)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	// ARRIVED, GONE, OR OFF THE TAXIWAYS: no longer unplanned (spec §3 - the alert clears on arrival).
	for (const int32 Id : TaxiPlanning->UnplannedHolders())
	{
		const FRoadAgent* Agent = FindAgent(Id);
		if (Agent == nullptr || (Agent->Phase != EAgentPhase::Taxiing && Agent->Phase != EAgentPhase::Arriving
			&& Agent->Phase != EAgentPhase::Manoeuvring))
		{
			TaxiPlanning->ClearUnplanned(Id);
		}
	}
	// THE REST TRIED AGAIN when the table has moved, or on the clock - a plan along the route it drives, from where it is now.
	for (const int32 Id : TaxiPlanning->UnplannedDueRetry(SimSeconds))
	{
		const FRoadAgent* Agent = FindAgent(Id);
		const FTaxiUnplanned* Record = TaxiPlanning->FindUnplanned(Id);
		if (Agent == nullptr || Record == nullptr || Agent->Phase != EAgentPhase::Taxiing)
		{
			continue;
		}
		TaxiPlanning->NoteUnplannedTried(Id, SimSeconds);
		const FString Why = Record->Why;
		ReplanTaxi(*Agent, Network, Record->Cause, *Why, nullptr);
	}
}
